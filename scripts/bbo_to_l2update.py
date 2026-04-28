#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    return parser.parse_args()

def main():
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    
    with args.input.open(newline="") as f_in, args.output.open("w", newline="") as f_out:
        reader = csv.DictReader(f_in)
        writer = csv.writer(f_out)
        writer.writerow(["timestamp", "is_snapshot", "is_bid", "price", "qty"])
        
        # Output initial snapshot
        first = True
        
        for row in reader:
            ts = row["timestamp"]
            b_p = row["bid_price"]
            b_q = row["bid_qty"]
            a_p = row["ask_price"]
            a_q = row["ask_qty"]
            
            is_snap = 1
            first = False
            
            # Emit bid
            writer.writerow([ts, is_snap, 1, b_p, b_q])
            # Emit ask
            writer.writerow([ts, is_snap, 0, a_p, a_q])

if __name__ == "__main__":
    main()
