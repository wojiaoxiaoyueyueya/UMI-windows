#!/usr/bin/env python3
"""将原始录制会话转换为 HDF5 v1.0 数据集格式。

用法：convert_to_hdf5.py <原始会话目录> <输出数据集目录> [--task 任务描述] [--progress 进度文件]

依赖：pip3 install h5py
"""

import json, os, sys, csv, shutil, math, struct

try:
    import h5py
    import numpy as np
except ImportError:
    sys.stderr.write("ERROR: h5py and numpy required. Install: pip3 install h5py numpy\n")
    sys.exit(3)


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
    is_electric = False
    if not os.path.exists(path):
        return rows, is_electric
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        headers = reader.fieldnames or []
        is_electric = 'position_deg' in headers
        for r in reader:
            if is_electric:
                rows.append({
                    'ts': aligned_ts(r),
                    'position_deg': float(r.get('position_deg', 0)),
                    'velocity': float(r.get('velocity_rpm', 0)),
                    'current': float(r.get('current_a', 0)),
                })
            else:
                if 'position' in r:
                    close_ratio = float(r['position'])
                else:
                    close_ratio = float(r['close_ratio'])
                rows.append({
                    'ts': aligned_ts(r),
                    'close_ratio': close_ratio,
                })
    rows.sort(key=lambda x: x['ts'])
    return rows, is_electric


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
        return np.zeros((n, 6), dtype=np.float32)
    states = np.zeros((n, 6), dtype=np.float32)
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(imu_rows, ft)
        if r:
            states[i] = [r['ax'], r['ay'], r['az'], r['gx'], r['gy'], r['gz']]
    return states


def align_gripper(gripper_rows, video_timestamps, device_offset_us, n, fps, is_electric=False):
    dim = 3 if is_electric else 1
    if not gripper_rows or n == 0:
        return np.zeros((n, dim), dtype=np.float32)
    converted = [{'ts': r['ts'] - device_offset_us, **{k: r[k] for k in r if k != 'ts'}} for r in gripper_rows]
    converted.sort(key=lambda x: x['ts'])
    states = np.zeros((n, dim), dtype=np.float32)
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(converted, ft)
        if r:
            if is_electric:
                states[i] = [r['position_deg'], r['velocity'], r['current']]
            else:
                states[i, 0] = r['close_ratio']
    return states


def align_pose(pose_rows, video_timestamps, device_offset_us, n, fps):
    if not pose_rows or n == 0:
        return np.zeros((n, 7), dtype=np.float32)
    converted = [{'ts': r['ts'] - device_offset_us, 'x': r['x'], 'y': r['y'], 'z': r['z'],
                  'qx': r['qx'], 'qy': r['qy'], 'qz': r['qz'], 'qw': r['qw']} for r in pose_rows]
    converted.sort(key=lambda x: x['ts'])
    states = np.zeros((n, 7), dtype=np.float32)
    for i in range(n):
        ft = _get_frame_ts(video_timestamps, i, fps)
        r = _binary_search_ts(converted, ft)
        if r:
            states[i] = [r['x'], r['y'], r['z'], r['qx'], r['qy'], r['qz'], r['qw']]
    return states


def convert_slot(session_id, fps, start_us, source_dir, output_dir, slot_meta, progress_file=None, slot_label=None):
    """将单个槽位转换为 HDF5；如果是旧版扁平目录，则转换整个会话。

    slot_meta: metadata dict for this slot (top-level meta for legacy, or
               meta['slots'][name] for new format).  May be None for legacy.
    """
    label = slot_label or session_id

    device_offset_us = (slot_meta or {}).get('deviceToSystemOffsetUs', 0)

    frame_counts = (slot_meta or {}).get('frameCount', {})
    imu_count = (slot_meta or {}).get('imuCount', 0)
    gripper_count = (slot_meta or {}).get('gripperCount', 0)
    pc_count = (slot_meta or {}).get('pointCloudCount', 0)

    # 判断夹爪 CSV 路径：新格式为 gripper.csv，旧格式为 gripper_data.csv。
    gripper_csv_new = os.path.join(source_dir, 'gripper_data', 'gripper.csv')
    gripper_csv_old = os.path.join(source_dir, 'gripper_data', 'gripper_data.csv')
    if os.path.exists(gripper_csv_new):
        gripper_csv = gripper_csv_new
    else:
        gripper_csv = gripper_csv_old

    has_imu = imu_count > 0 and os.path.exists(os.path.join(source_dir, 'imu_data', 'imu_data.csv'))
    has_gripper = gripper_count > 0 and os.path.exists(gripper_csv)
    pose_path = os.path.join(source_dir, 'pose_data', 'pose_data.csv')
    has_pose = os.path.exists(pose_path) and ((slot_meta or {}).get('pose', {}).get('frames', 0) > 0)

    available_videos = []
    video_info = {}
    for vtype, vinfo in VIDEO_MAP.items():
        src = os.path.join(source_dir, vinfo['dir'], vinfo['file'])
        cnt = frame_counts.get(vtype, 0)
        if cnt > 0 and os.path.exists(src):
            available_videos.append(vtype)
            vi = ((slot_meta or {}).get('videos', {}) or {}).get(vtype) or {}
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
        sys.stderr.write(f"SKIP: no convertible data in slot {label}\n")
        return False

    if nframes == 0:
        sys.stderr.write(f"SKIP: no data frames in slot {label}\n")
        return False

    video_timestamps = []
    if primary_video:
        video_timestamps = read_video_timestamps(source_dir, primary_video)
    if not video_timestamps or len(video_timestamps) != nframes:
        video_timestamps = [start_us + int(j * 1_000_000 / fps) for j in range(nframes)]

    os.makedirs(output_dir, exist_ok=True)

    # 读取传感器数据，并按视频帧时间戳对齐。
    write_progress(progress_file, 0.15, f"Reading sensor data [{label}]...")
    imu_rows = read_imu(os.path.join(source_dir, 'imu_data', 'imu_data.csv'))
    imu_states = align_imu(imu_rows, video_timestamps, nframes, fps) if has_imu else np.zeros((nframes, 6), dtype=np.float32)

    gripper_rows, is_electric_gripper = read_gripper(gripper_csv)
    eg_dim = 3 if is_electric_gripper else 1
    gripper_states = align_gripper(gripper_rows, video_timestamps, device_offset_us, nframes, fps, is_electric_gripper) if has_gripper else np.zeros((nframes, eg_dim), dtype=np.float32)

    pose_rows = read_pose(pose_path) if has_pose else []
    pose_states = align_pose(pose_rows, video_timestamps, device_offset_us, nframes, fps) if has_pose else np.zeros((nframes, 7), dtype=np.float32)

    # 构建 observation.state，将夹爪和可选位姿合并为状态向量。
    state_parts = [imu_states]
    if has_gripper:
        state_parts.append(gripper_states)
    if has_pose:
        state_parts.append(pose_states)
    obs_state = np.hstack(state_parts)

    action = np.zeros((nframes, 7), dtype=np.float32)
    timestamps = np.array([ts / 1_000_000.0 for ts in video_timestamps], dtype=np.float64)
    episode_index = np.zeros(nframes, dtype=np.int32)
    frame_index = np.arange(nframes, dtype=np.int32)

    # 复制视频文件，保持转换结果可以独立查看。
    for vtype in available_videos:
        vid_d = os.path.join(output_dir, 'videos', vtype)
        os.makedirs(vid_d, exist_ok=True)
        shutil.copy2(video_info[vtype]['src'], os.path.join(vid_d, video_info[vtype]['file']))

    # 写入 HDF5 主数据文件。
    write_progress(progress_file, 0.40, f"Writing HDF5 [{label}]...")
    hdf5_path = os.path.join(output_dir, 'data.hdf5')
    with h5py.File(hdf5_path, 'w') as hf:
        hf.attrs['format'] = 'HDF5'
        hf.attrs['version'] = 'v1.0'
        hf.attrs['session_id'] = session_id
        hf.attrs['fps'] = fps
        hf.attrs['robot_type'] = 'orbbec_umi_gripper'

        obs_grp = hf.create_group('observations')
        if available_videos:
            vid_grp = obs_grp.create_group('images')
            for vtype in available_videos:
                vid_grp.attrs[vtype] = json.dumps({
                    'width': video_info[vtype]['width'],
                    'height': video_info[vtype]['height'],
                    'frames': video_info[vtype]['frames'],
                    'file': f'videos/{vtype}/{video_info[vtype]["file"]}',
                })

        obs_grp.create_dataset('state', data=obs_state)
        obs_grp['state'].attrs['dim_labels'] = json.dumps(_state_labels(has_imu, has_gripper, has_pose, is_electric_gripper))

        hf.create_dataset('action', data=action)
        hf.create_dataset('timestamp', data=timestamps)
        hf.create_dataset('episode_index', data=episode_index)
        hf.create_dataset('frame_index', data=frame_index)

    # 写入 metadata.json，供看板和后续处理读取。
    write_progress(progress_file, 0.85, f"Writing metadata [{label}]...")
    out_meta = {
        'format': 'hdf5',
        'version': 'v1.0',
        'sessionId': session_id,
        'fps': fps,
        'totalFrames': nframes,
        'stateDim': obs_state.shape[1],
        'actionDim': 7,
        'hasIMU': has_imu,
        'hasGripper': has_gripper,
        'hasPose': has_pose,
        'videos': {vtype: {'width': video_info[vtype]['width'], 'height': video_info[vtype]['height'],
                           'frames': video_info[vtype]['frames'], 'file': video_info[vtype]['file']}
                   for vtype in available_videos},
    }
    if slot_meta and 'position' in slot_meta:
        out_meta['slotPosition'] = slot_meta['position']
        out_meta['deviceType'] = slot_meta.get('deviceType', '')
    with open(os.path.join(output_dir, 'metadata.json'), 'w') as f:
        json.dump(out_meta, f, indent=2, ensure_ascii=False)

    print(f"OK:{session_id}:{label}:{nframes}:hdf5:{fps}")
    return True


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.stderr.write("Usage: convert_to_hdf5.py <source_dir> <output_dir> [--task desc] [--progress file]\n")
        sys.exit(1)

    source_dir = args[0]
    output_dir = args[1]
    progress_file = ""

    i = 2
    while i < len(args):
        if args[i] == '--progress' and i + 1 < len(args):
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

    slots = meta.get('slots')

    if slots:
        # 新格式：按槽位分目录。
        total_slots = len(slots)
        ok_count = 0
        for idx, (slot_name, slot_meta) in enumerate(slots.items()):
            slot_dir = os.path.join(source_dir, slot_name)
            slot_out = os.path.join(output_dir, slot_name)
            # 多槽位转换时按槽位数量分摊进度。
            if progress_file:
                slot_pf = progress_file  # reuse same file; convert_slot writes its own progress
            else:
                slot_pf = None
            write_progress(progress_file, idx / total_slots, f"Converting slot {slot_name}...")
            if convert_slot(session_id, fps, start_us, slot_dir, slot_out, slot_meta,
                            progress_file=slot_pf, slot_label=slot_name):
                ok_count += 1
        write_progress(progress_file, 1.0, "Done", done=True)
        if ok_count == 0:
            sys.stderr.write("SKIP: no slots produced output\n")
            sys.exit(2)
    else:
        # 旧格式：扁平目录，保留向后兼容。
        ok = convert_slot(session_id, fps, start_us, source_dir, output_dir, meta,
                          progress_file=progress_file, slot_label=None)
        if not ok:
            sys.exit(2)
        write_progress(progress_file, 1.0, "Done", done=True)


def _state_labels(has_imu, has_gripper, has_pose, is_electric=False):
    labels = []
    if has_imu:
        labels += ['accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z']
    if has_gripper:
        if is_electric:
            labels += ['position_deg', 'velocity_rpm', 'current_a']
        else:
            labels += ['close_ratio']
    if has_pose:
        labels += ['pos_x', 'pos_y', 'pos_z', 'quat_x', 'quat_y', 'quat_z', 'quat_w']
    return labels


if __name__ == '__main__':
    main()
