#!/usr/bin/env python3
"""Re-render the 9 "UnMasking Exports" settings through the CLI and score them
with the speech-band metrics (2026-11-04; see HANDOFF_speech_band_sii.md).

The exports themselves are mix-only browser renders, so separated margin and
SII can't be computed from them. Each folder's settings.txt is parsed
(compute_folder_export_metrics.parse_settings: threshold, ratio, knee, attack,
release, max reduction) and the same setting is rendered as Basic and Advanced
(Summed-bus) with `--split-out`. compute_folder_export_metrics.py and
folder_export_results.csv are unchanged.

Usage:
    python3 analysis/rerender_folder_exports.py [--out analysis/folder_export_speech.csv]
"""
import argparse
import csv
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compute_folder_export_metrics import (SCENE_DIR_BY_LABEL, SCENE_STEMS,  # noqa: E402
                                           name_implied_threshold, parse_settings)
from compute_metrics import load_mono  # noqa: E402
from split_metrics import gain_columns, render_split, score_split  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
MODES = ['basic', 'advanced']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exports-dir', default='UnMasking Exports')
    ap.add_argument('--out', default='analysis/folder_export_speech.csv')
    args = ap.parse_args()

    export_dir = REPO_ROOT / args.exports_dir
    folders = sorted(d for d in export_dir.iterdir() if d.is_dir() and (d / 'settings.txt').exists())
    tmp = Path(tempfile.mkdtemp(prefix="folder_exports_"))
    baselines, rows = {}, []
    for folder in folders:
        s = parse_settings((folder / 'settings.txt').read_text())
        scene_dir = SCENE_DIR_BY_LABEL[s['scene_label']]
        key_label = s['key_label']
        if s['scene_label'] == 'Restaurant':
            print(f"skipping {folder.name} (Restaurant excluded)")
            continue
        dry, sr = load_mono(REPO_ROOT / scene_dir / SCENE_STEMS[scene_dir][key_label])
        if (scene_dir, key_label) not in baselines:
            baselines[(scene_dir, key_label)] = score_split(
                render_split(scene_dir, key_label, 'unprocessed', tmp / 'unprocessed'), dry, sr)
        base = baselines[(scene_dir, key_label)]
        thr_name = name_implied_threshold(folder.name)
        row = {'folder': folder.name, 'scene': s['scene_label'], 'key': key_label,
               'name_settings_mismatch': thr_name is not None and thr_name != s['threshold_db'],
               **{k: s[k] for k in ('threshold_db', 'ratio', 'knee_db', 'attack_ms', 'release_ms',
                                    'max_reduction_db')}}
        extra = ["--threshold", s['threshold_db'], "--ratio", s['ratio'], "--knee", s['knee_db'],
                 "--attack", s['attack_ms'], "--release", s['release_ms'],
                 "--max-reduction", s['max_reduction_db']]
        for mode in MODES:
            prefix = render_split(scene_dir, key_label, mode, tmp / mode, extra)
            row.update(gain_columns(mode, score_split(prefix, dry, sr), base))
        rows.append(row)
        print(f"{folder.name}: " + "  ".join(
            f"{m} margin {row[f'{m}_speech_margin_db_gain']:+.2f} dB ESII {row[f'{m}_esii_gain']:+.3f} "
            f"ESTOI {row[f'{m}_estoi_gain']:+.3f}" for m in MODES), flush=True)

    out_path = REPO_ROOT / args.out
    if out_path.exists():
        sys.exit(f"{out_path} already exists; remove it or pass --out.")
    with open(out_path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"Saved {len(rows)} rows to {out_path}")


if __name__ == '__main__':
    main()
