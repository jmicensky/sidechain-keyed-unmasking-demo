#!/usr/bin/env python3
"""Computes masking-margin + STOI metrics for "Export All Examples" folder
exports (see web/app.js's exportAllExamples() / HANDOFF.md), one row per
export folder. Parses each folder's settings.txt for the swept compressor
parameters (threshold, ratio, knee, attack, release) - NOT the folder name,
which can drift from what was actually exported (this script flags any
such mismatch rather than silently trusting the name).

Reuses the masking-margin/STOI implementation from compute_metrics.py
rather than duplicating it.

Usage:
    python3 analysis/compute_folder_export_metrics.py \
        [--exports-dir "UnMasking Exports"] [--out analysis/folder_export_results.csv]
"""
import argparse
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_metrics import (  # noqa: E402
    load_mono, CLASS_RANGES, DEFAULT_SAFETY_GAIN_DB,
    compute_masking_margin, compute_stoi, HAVE_PYSTOI,
)

REPO_ROOT = Path(__file__).resolve().parent.parent

# settings.txt records the scene/key as their UI display text (see
# web/app.js's buildExportManifest()), not the slugs compute_metrics.py's
# SCENES/CLASS_RANGES use - map between them here.
SCENE_DIR_BY_LABEL = {
    'Construction': 'Construction Scene',
    'Public Transit': 'PublicTransit Scene',
}
SCENE_STEMS = {
    'Construction Scene': {
        'Dialogue': 'Dialogue.wav', 'Music': 'MUSIC.wav', 'Background Noise': 'BKG.wav',
        'Safety Alerts': 'SAFETY.wav', 'Other': 'OTHER.wav',
    },
    'PublicTransit Scene': {
        'Dialogue': 'Dialogue_PublicTransit_01.wav', 'Music': 'MUSIC_publictransit_01.wav',
        'Background Noise': 'BKG_PublicTransit_01.wav', 'Safety Alerts': 'SAFETY_publictransit_01.wav',
        'Other': 'OTHER_publictransit_01.wav',
    },
}
KEY_SLUG = {'Dialogue': 'dialogue', 'Music': 'music', 'Background Noise': 'background',
            'Safety Alerts': 'safety', 'Other': 'other'}


def parse_settings(text):
    def grab(pattern, cast=str):
        m = re.search(pattern, text)
        return cast(m.group(1)) if m else None
    return {
        'scene_label': grab(r'Scene:\s*(.+)'),
        'key_label': grab(r'Key channel:\s*(.+)'),
        'threshold_db': grab(r'Threshold:\s*(-?\d+(?:\.\d+)?)dB', float),
        'ratio': grab(r'Ratio:\s*(-?\d+(?:\.\d+)?):1', float),
        'knee_db': grab(r'Knee:\s*(-?\d+(?:\.\d+)?)dB', float),
        'attack_ms': grab(r'Attack:\s*(-?\d+(?:\.\d+)?)ms', float),
        'release_ms': grab(r'Release:\s*(-?\d+(?:\.\d+)?)ms', float),
        'max_reduction_db': grab(r'Max Reduction:\s*(-?\d+(?:\.\d+)?)dB', float),
    }


def name_implied_threshold(folder_name):
    m = re.match(r'^(-?\d+)Thesh', folder_name)
    return float(m.group(1)) if m else None


def fmt(x, unit=''):
    return f'{x:.2f}{unit}' if x is not None else 'n/a'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exports-dir', default='UnMasking Exports')
    parser.add_argument('--out', default='analysis/folder_export_results.csv')
    args = parser.parse_args()

    export_dir = Path(args.exports_dir)
    if not export_dir.is_absolute():
        export_dir = REPO_ROOT / export_dir

    folders = sorted(d for d in export_dir.iterdir() if d.is_dir() and (d / 'settings.txt').exists())
    if not folders:
        print(f'No export folders with settings.txt found in {export_dir}', file=sys.stderr)
        sys.exit(1)

    if not HAVE_PYSTOI:
        print('WARNING: pystoi not installed - STOI scores will be skipped.', file=sys.stderr)

    rows = []
    for folder in folders:
        text = (folder / 'settings.txt').read_text()
        settings = parse_settings(text)
        scene_label = settings['scene_label']
        key_label = settings['key_label']
        scene_dir_name = SCENE_DIR_BY_LABEL.get(scene_label)
        key_slug = KEY_SLUG.get(key_label)
        if scene_dir_name is None or key_slug is None:
            print(f'WARNING: unrecognized scene/key in {folder.name} ({scene_label}/{key_label}) - skipping', file=sys.stderr)
            continue

        dry_path = REPO_ROOT / scene_dir_name / SCENE_STEMS[scene_dir_name][key_label]
        dry, dry_sr = load_mono(dry_path)
        low_hz, high_hz = CLASS_RANGES[key_slug]

        name_thr = name_implied_threshold(folder.name)
        name_mismatch = name_thr is not None and name_thr != settings['threshold_db']

        processed_files = sorted(folder.glob('processed_*.wav'))
        conditions = {'unprocessed': folder / 'unprocessed.wav', 'basic': folder / 'basic.wav'}
        if processed_files:
            conditions['processed'] = processed_files[0]
            row_processed_slug = processed_files[0].stem.replace('processed_', '')
        else:
            row_processed_slug = ''

        row = {
            'folder': folder.name,
            'name_settings_mismatch': name_mismatch,
            'processed_mode_slug': row_processed_slug,
            'scene': scene_label,
            'key': key_label,
            **settings,
        }

        for cond_name, cond_path in conditions.items():
            if not cond_path.exists():
                continue
            mix, mix_sr = load_mono(cond_path)
            if mix_sr != dry_sr:
                print(f'WARNING: sample rate mismatch in {folder.name}/{cond_path.name}', file=sys.stderr)
                continue
            margin = compute_masking_margin(dry, mix, dry_sr, low_hz, high_hz, DEFAULT_SAFETY_GAIN_DB)
            row[f'{cond_name}_mean_margin_db'] = margin['mean_margin_db'] if margin else None
            row[f'{cond_name}_worst_margin_db'] = margin['worst_margin_db'] if margin else None
            if key_slug == 'dialogue' and HAVE_PYSTOI:
                score, _lag = compute_stoi(dry, mix, dry_sr, extended=True)
                row[f'{cond_name}_stoi'] = score
            else:
                row[f'{cond_name}_stoi'] = None

        rows.append(row)
        mismatch_flag = '  [FOLDER NAME DOES NOT MATCH settings.txt]' if name_mismatch else ''
        print(f"{folder.name}{mismatch_flag}")
        print(f"  thr={settings['threshold_db']}dB ratio={settings['ratio']}:1 knee={settings['knee_db']}dB "
              f"atk={settings['attack_ms']}ms rel={settings['release_ms']}ms")
        print(f"  margin (mean): unprocessed={fmt(row.get('unprocessed_mean_margin_db'), 'dB')}  "
              f"basic={fmt(row.get('basic_mean_margin_db'), 'dB')}  processed={fmt(row.get('processed_mean_margin_db'), 'dB')}")
        if row.get('processed_stoi') is not None:
            print(f"  STOI: unprocessed={fmt(row.get('unprocessed_stoi'))}  basic={fmt(row.get('basic_stoi'))}  processed={fmt(row.get('processed_stoi'))}")
        print()

    out_path = REPO_ROOT / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = []
    for r in rows:
        for k in r.keys():
            if k not in fieldnames:
                fieldnames.append(k)
    with open(out_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(f'Saved to {out_path}')


if __name__ == '__main__':
    main()
