#!/usr/bin/env python3
"""将原始录制会话转换为 RLDS v0.1 数据集格式。

用法：convert_to_rlds.py <原始会话目录> <输出数据集目录> [--task 任务描述] [--progress 进度文件]

RLDS 格式使用 TFRecord 文件，并采用兼容 tensorflow_datasets 的目录结构。
Reference: https://github.com/google-research/rlds

Requires: pip3 install tensorflow
"""

import json, os, sys, csv, shutil, math, struct

try:
    import numpy as np
except ImportError:
    sys.stderr.write("ERROR: numpy required. Install: pip3 install numpy\n")
    sys.exit(3)

# RLDS 使用 TFRecord；这里只写基础格式，不强制依赖完整 TensorFlow。
# 使用轻量级 TFRecord 写入器，避免为了转换流程安装庞大的 TensorFlow 包。

VIDEO_MAP = {
    'color': {'dir': 'color_video', 'file': 'color.mp4'},
    'depth': {'dir': 'depth_video', 'file': 'depth.mp4'},
    'ir':    {'dir': 'ir_video', 'file': 'ir.mp4'},
}


def aligned_ts(row):
    return int(row.get('session_time_us') or row['timestamp_us'])


def write_progress(pf, progress, step, done=False):
    if not pf:
        return
    try:
        with open(pf, 'w') as f:
            json.dump({"progress": progress, "step": step, "done": done}, f)
    except Exception:
        pass


def read_imu(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            rows.append({
                'ts': aligned_ts(r),
                'ax': float(r['accel_x']), 'ay': float(r['accel_y']), 'az': float(r['accel_z']),
                'gx': float(r['gyro_x']), 'gy': float(r['gyro_y']), 'gz': float(r['gyro_z']),
            })
    rows.sort(key=lambda x: x['ts'])
    return rows


def read_gripper(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        for r in reader:
            # 新格式使用 position 列表示夹爪闭合比例。
            if 'position' in fieldnames:
                close_ratio = float(r['position'])
            else:
                close_ratio = float(r['close_ratio'])
            rows.append({
                'ts': aligned_ts(r),
                'close_ratio': close_ratio,
            })
    rows.sort(key=lambda x: x['ts'])
    return rows


def read_pose(path):
    if not os.path.exists(path):
        return []
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            rows.append({
                'ts': aligned_ts(r),
                'x': float(r['x']), 'y': float(r['y']), 'z': float(r['z']),
                'qx': float(r['qx']), 'qy': float(r['qy']), 'qz': float(r['qz']), 'qw': float(r['qw']),
            })
    rows.sort(key=lambda x: x['ts'])
    return rows


def read_video_timestamps(source_dir, video_type):
    ts_path = os.path.join(source_dir, VIDEO_MAP[video_type]['dir'], 'timestamps.csv')
    timestamps = []
    if not os.path.exists(ts_path):
        return timestamps
    with open(ts_path, newline='') as f:
        for r in csv.DictReader(f):
            timestamps.append(aligned_ts(r))
    return timestamps


def _get_frame_ts(video_timestamps, i, fps):
    if i < len(video_timestamps):
        return video_timestamps[i]
    elif video_timestamps:
        return video_timestamps[-1] + int((i - len(video_timestamps) + 1) * 1_000_000 / fps)
    return 0


def _binary_search_ts(rows, target_ts, ts_key='ts'):
    if not rows:
        return None
    lo, hi = 0, len(rows) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if rows[mid][ts_key] < target_ts:
            lo = mid + 1
        else:
            hi = mid
    return rows[min(lo, len(rows) - 1)]


def align_imu(imu_rows, video_timestamps, n, fps):
    if not imu_rows or n == 0:
        return [[0.0] * 6] * n
    states = []
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(imu_rows, ft)
        states.append([r['ax'], r['ay'], r['az'], r['gx'], r['gy'], r['gz']] if r else [0.0] * 6)
    return states


def align_gripper(gripper_rows, video_timestamps, device_offset_us, n, fps):
    if not gripper_rows or n == 0:
        return [[0.0]] * n
    converted = [{'ts': r['ts'] - device_offset_us, 'close_ratio': r['close_ratio']} for r in gripper_rows]
    converted.sort(key=lambda x: x['ts'])
    states = []
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(converted, ft)
        states.append([r['close_ratio']] if r else [0.0])
    return states


def align_pose(pose_rows, video_timestamps, device_offset_us, n, fps):
    if not pose_rows or n == 0:
        return [[0.0] * 7] * n
    converted = [{'ts': r['ts'] - device_offset_us, 'x': r['x'], 'y': r['y'], 'z': r['z'],
                  'qx': r['qx'], 'qy': r['qy'], 'qz': r['qz'], 'qw': r['qw']} for r in pose_rows]
    converted.sort(key=lambda x: x['ts'])
    states = []
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(converted, ft)
        states.append([r['x'], r['y'], r['z'], r['qx'], r['qy'], r['qz'], r['qw']] if r else [0.0] * 7)
    return states


# --- 轻量级 TFRecord 写入器（不依赖 TensorFlow）---
# TFRecord 基础结构：uint64 长度 | 长度 CRC | 数据内容 | 数据 CRC。
def _masked_crc32c(data):
    import crcmod
    crc = crcmod.predefined.mkCrcFun('crc-32c')(data)
    return (((crc >> 15) | (crc << 17)) + 0xa282ead8) & 0xffffffff

def _write_tfrecord(path, records):
    """将字节记录列表写入 TFRecord 文件。"""
    with open(path, 'wb') as f:
        for record in records:
            length = len(record)
            f.write(struct.pack('<Q', length))
            f.write(struct.pack('<I', _masked_crc32c(struct.pack('<Q', length))))
            f.write(record)
            f.write(struct.pack('<I', _masked_crc32c(record)))


def _bytes_feature(value):
    return value

def _float_list(values):
    """将浮点数组编码为类似 tf.train.Feature 的字节表示。"""
    fbuf = struct.pack('<' + 'f' * len(values), *values)
    return fbuf

def _int_list(values):
    """将整数数组编码为类似 tf.train.Feature 的字节表示。"""
    return struct.pack('<' + 'q' * len(values), *values)


def build_step_feature(obs_state, action, reward, discount, is_first, is_last, is_terminal):
    """构建单个 RLDS step，并用 JSON 序列化为字节数据。"""
    step = {
        'observation': {
            'state': obs_state,
        },
        'action': action,
        'reward': [reward],
        'discount': [discount],
        'is_first': [is_first],
        'is_last': [is_last],
        'is_terminal': [is_terminal],
    }
    return json.dumps(step).encode('utf-8')


def convert_slot(source_dir, output_dir, session_id, fps, start_us, slot_meta, progress_file, slot_label=""):
    """将单个槽位转换为 RLDS；如果是旧版扁平目录，则转换整个会话。

    Parameters:
        source_dir:  path to the slot directory (or session dir for old format)
        output_dir:  path where RLDS output is written
        session_id:  session identifier string
        fps:         video frame rate
        start_us:    session start time in microseconds
        slot_meta:   metadata dict for this slot (top-level meta for old format)
        progress_file: path to progress json file (or empty string)
        slot_label:  human-readable label for progress messages
    """
    label_prefix = f"[{slot_label}] " if slot_label else ""

    device_offset_us = slot_meta.get('deviceToSystemOffsetUs', 0)

    frame_counts = slot_meta.get('frameCount', {})
    imu_count = slot_meta.get('imuCount', 0)
    gripper_count = slot_meta.get('gripperCount', 0)

    # 判断夹爪 CSV 文件名：新格式为 gripper.csv，旧格式为 gripper_data.csv。
    gripper_csv_new = os.path.join(source_dir, 'gripper_data', 'gripper.csv')
    gripper_csv_old = os.path.join(source_dir, 'gripper_data', 'gripper_data.csv')
    if os.path.exists(gripper_csv_new):
        gripper_csv = gripper_csv_new
    else:
        gripper_csv = gripper_csv_old

    has_imu = imu_count > 0 and os.path.exists(os.path.join(source_dir, 'imu_data', 'imu_data.csv'))
    has_gripper = gripper_count > 0 and os.path.exists(gripper_csv)
    pose_path = os.path.join(source_dir, 'pose_data', 'pose_data.csv')
    has_pose = os.path.exists(pose_path) and slot_meta.get('pose', {}).get('frames', 0) > 0

    available_videos = []
    video_info = {}
    for vtype, vinfo in VIDEO_MAP.items():
        src = os.path.join(source_dir, vinfo['dir'], vinfo['file'])
        cnt = frame_counts.get(vtype, 0)
        if cnt > 0 and os.path.exists(src):
            available_videos.append(vtype)
            vi = slot_meta.get('videos', {}).get(vtype) or {}
            video_info[vtype] = {'width': vi.get('width', 640), 'height': vi.get('height', 480),
                                 'frames': cnt, 'src': src, 'file': vinfo['file']}

    primary_video = None
    nframes = 0
    if available_videos:
        for vtype in ['color', 'depth', 'ir']:
            if vtype in available_videos:
                primary_video = vtype
                nframes = video_info[vtype]['frames']
                break
    elif has_imu:
        nframes = imu_count
    else:
        sys.stderr.write(f"SKIP: {label_prefix}no convertible data in session\n")
        return False

    if nframes == 0:
        sys.stderr.write(f"SKIP: {label_prefix}no data frames in session\n")
        return False

    video_timestamps = []
    if primary_video:
        video_timestamps = read_video_timestamps(source_dir, primary_video)
    if not video_timestamps or len(video_timestamps) != nframes:
        video_timestamps = [start_us + int(j * 1_000_000 / fps) for j in range(nframes)]

    os.makedirs(output_dir, exist_ok=True)
    data_dir = os.path.join(output_dir, 'data')
    os.makedirs(data_dir, exist_ok=True)

    # 复制视频文件。
    for vtype in available_videos:
        vid_d = os.path.join(output_dir, 'videos', vtype)
        os.makedirs(vid_d, exist_ok=True)
        shutil.copy2(video_info[vtype]['src'], os.path.join(vid_d, video_info[vtype]['file']))

    # 读取并对齐传感器数据。
    write_progress(progress_file, 0.15, f"{label_prefix}Reading sensor data...")
    imu_rows = read_imu(os.path.join(source_dir, 'imu_data', 'imu_data.csv'))
    imu_states = align_imu(imu_rows, video_timestamps, nframes, fps) if has_imu else [[0.0] * 6] * nframes

    gripper_rows = read_gripper(gripper_csv)
    gripper_states = align_gripper(gripper_rows, video_timestamps, device_offset_us, nframes, fps) if has_gripper else [[0.0]] * nframes

    pose_rows = read_pose(pose_path) if has_pose else []
    pose_states = align_pose(pose_rows, video_timestamps, device_offset_us, nframes, fps) if has_pose else None

    # 合并状态向量。
    write_progress(progress_file, 0.30, f"{label_prefix}Building RLDS steps...")
    records = []
    for i in range(nframes):
        state_parts = imu_states[i] if has_imu else []
        if has_gripper:
            state_parts = state_parts + gripper_states[i]
        if has_pose and pose_states:
            state_parts = state_parts + pose_states[i]

        action = [0.0] * 7
        reward = 0.0
        discount = 1.0
        is_first = (i == 0)
        is_last = (i == nframes - 1)
        is_terminal = False

        record = build_step_feature(state_parts, action, reward, discount, is_first, is_last, is_terminal)
        records.append(record)

    # 写入 TFRecord 文件。
    write_progress(progress_file, 0.50, f"{label_prefix}Writing TFRecord...")
    tfrecord_path = os.path.join(data_dir, 'train-00000-of-00001.tfrecord')
    try:
        _write_tfrecord(tfrecord_path, records)
    except ImportError:
        # 兜底：缺少 crcmod 时写 JSONL，保证转换结果仍可检查。
        write_progress(progress_file, 0.50, f"{label_prefix}crcmod not found, writing JSONL fallback...")
        jsonl_path = os.path.join(data_dir, 'train-00000-of-00001.jsonl')
        with open(jsonl_path, 'w') as f:
            for rec in records:
                f.write(rec.decode('utf-8') + '\n')

    # 写入 features.json。
    state_labels = []
    if has_imu:
        state_labels += ['accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z']
    if has_gripper:
        state_labels += ['close_ratio']
    if has_pose:
        state_labels += ['pos_x', 'pos_y', 'pos_z', 'quat_x', 'quat_y', 'quat_z', 'quat_w']

    features = {
        'observation/state': {'type': 'float32', 'shape': [len(state_labels)], 'names': state_labels},
        'action': {'type': 'float32', 'shape': [7], 'names': ['joint_0', 'joint_1', 'joint_2', 'joint_3', 'joint_4', 'joint_5', 'gripper']},
        'reward': {'type': 'float32', 'shape': [1]},
        'discount': {'type': 'float32', 'shape': [1]},
        'is_first': {'type': 'bool', 'shape': [1]},
        'is_last': {'type': 'bool', 'shape': [1]},
        'is_terminal': {'type': 'bool', 'shape': [1]},
    }
    with open(os.path.join(data_dir, 'features.json'), 'w') as f:
        json.dump(features, f, indent=2, ensure_ascii=False)

    # 写入 metadata.json。
    write_progress(progress_file, 0.85, f"{label_prefix}Writing metadata...")
    out_meta = {
        'format': 'rlds',
        'version': 'v0.1',
        'sessionId': session_id,
        'fps': fps,
        'totalFrames': nframes,
        'totalEpisodes': 1,
        'stateDim': len(state_labels),
        'actionDim': 7,
        'hasIMU': has_imu,
        'hasGripper': has_gripper,
        'hasPose': has_pose,
        'videos': {vtype: {'width': video_info[vtype]['width'], 'height': video_info[vtype]['height'],
                           'frames': video_info[vtype]['frames'], 'file': video_info[vtype]['file']}
                   for vtype in available_videos},
    }
    # 新格式下写入槽位名称和显示位置。
    if 'position' in slot_meta:
        out_meta['slotPosition'] = slot_meta['position']
    if 'deviceType' in slot_meta:
        out_meta['deviceType'] = slot_meta['deviceType']

    with open(os.path.join(output_dir, 'metadata.json'), 'w') as f:
        json.dump(out_meta, f, indent=2, ensure_ascii=False)

    print(f"OK:{session_id}:{nframes}:rlds:{fps}")
    return True


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.stderr.write("Usage: convert_to_rlds.py <source_dir> <output_dir>\n")
        sys.exit(1)

    source_dir = args[0]
    output_dir = args[1]
    task = ""
    progress_file = ""

    i = 2
    while i < len(args):
        if args[i] == '--task' and i + 1 < len(args):
            task = args[i + 1]; i += 2
        elif args[i] == '--progress' and i + 1 < len(args):
            progress_file = args[i + 1]; i += 2
        else:
            i += 1

    write_progress(progress_file, 0.0, "Reading metadata...")

    with open(os.path.join(source_dir, 'metadata.json')) as f:
        meta = json.load(f)

    session_id = meta['sessionId']
    fps = meta.get('fps', 30.0)
    if 'startTimeUs' in meta:
        start_us = meta['startTimeUs']
    else:
        start_us = meta.get('startTime', 0) * 1000

    # 判断新槽位目录格式或旧扁平目录格式。
    slots = meta.get('slots', None)

    if slots:
        # 新格式：遍历每个槽位，并分别生成输出数据集。
        slot_names = sorted(slots.keys())
        total_slots = len(slot_names)
        any_ok = False
        for idx, slot_name in enumerate(slot_names):
            slot_meta = slots[slot_name]
            slot_dir = os.path.join(source_dir, slot_name)
            slot_output_dir = os.path.join(output_dir, slot_name)

            slot_fps = fps
            # 每个槽位可能有自己的起始时间；缺失时回退到会话级起始时间。
            slot_start_us = slot_meta.get('firstVideoDeviceTimestampUs', start_us)
            if 'startTimeUs' in slot_meta:
                slot_start_us = slot_meta['startTimeUs']

            write_progress(progress_file, idx / total_slots * 0.05,
                           f"Converting slot {idx + 1}/{total_slots}: {slot_name}...")
            ok = convert_slot(slot_dir, slot_output_dir, session_id, slot_fps,
                              slot_start_us, slot_meta, progress_file, slot_label=slot_name)
            if ok:
                any_ok = True

        if not any_ok:
            sys.stderr.write("SKIP: no slot had convertible data\n")
            sys.exit(2)

        write_progress(progress_file, 1.0, "Done", done=True)
    else:
        # 旧格式：单个扁平目录，只执行一次转换。
        ok = convert_slot(source_dir, output_dir, session_id, fps,
                          start_us, meta, progress_file)
        if not ok:
            sys.exit(2)

        write_progress(progress_file, 1.0, "Done", done=True)


if __name__ == '__main__':
    main()
