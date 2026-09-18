import sys
import pandas as pd
import matplotlib.pyplot as plt
import os

def plot_latencies(csv_file):
    try:
        df = pd.read_csv(csv_file)
    except Exception as e:
        print(f"Failed to load {csv_file}: {e}")
        return

    if df.empty or 'latency_cycles' not in df.columns:
        print("No valid latency data found to plot.")
        return

    os.makedirs('results', exist_ok=True)
    
    # 1. Latency by Request
    plt.figure(figsize=(10, 6))
    plt.plot(df['request_id'], df['latency_cycles'], marker='o', linestyle='-')
    plt.title('Translation Latency by Request')
    plt.xlabel('Request ID')
    plt.ylabel('Latency (Cycles)')
    plt.grid(True)
    plt.savefig('results/latency_by_request.png')
    plt.close()

    # 2. Latency Distribution (Histogram)
    plt.figure(figsize=(10, 6))
    plt.hist(df['latency_cycles'].dropna(), bins=20, color='blue', edgecolor='black')
    plt.title('Latency Distribution')
    plt.xlabel('Latency (Cycles)')
    plt.ylabel('Frequency')
    plt.grid(True)
    plt.savefig('results/latency_distribution.png')
    plt.close()

    # 3. Hit vs Miss Latency
    is_hit = (df['iotlb_hit'] == True) | (df['iotlb_hit'].astype(str).str.upper() == 'YES') | (df['iotlb_miss'] == False) | (df['iotlb_miss'].astype(str).str.upper() == 'NO')
    is_miss = (df['iotlb_miss'] == True) | (df['iotlb_miss'].astype(str).str.upper() == 'YES')
    hit_lats = df[is_hit]['latency_cycles'].dropna()
    miss_lats = df[is_miss]['latency_cycles'].dropna()
    
    plt.figure(figsize=(10, 6))
    plt.boxplot([hit_lats, miss_lats])
    plt.xticks([1, 2], ['IOTLB Hit', 'IOTLB Miss'])
    plt.title('Latency: IOTLB Hit vs Miss')
    plt.ylabel('Latency (Cycles)')
    plt.grid(True)
    plt.savefig('results/latency_hit_vs_miss.png')
    plt.close()

    print("[+] Generated plots in results/ directory:")
    print("    - results/latency_by_request.png")
    print("    - results/latency_distribution.png")
    print("    - results/latency_hit_vs_miss.png")

if __name__ == '__main__':
    csv_path = sys.argv[1] if len(sys.argv) > 1 else 'results/translation_trace.csv'
    plot_latencies(csv_path)
