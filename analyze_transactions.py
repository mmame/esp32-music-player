import re
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime

# Read the log file
log_file = r"C:\Users\default.DESKTOP-FC9SLHI\AppData\Local\Temp\TimApi\TransactionTrace_20260113_150019.log"

with open(log_file, 'r') as f:
    lines = f.readlines()

# Parse transaction completion times
transactions = []
pattern = r'\[(\d{2}:\d{2}:\d{2}\.\d{3})\] Transaction #(\d+) completed \(Duration: ([\d.]+)s\)'

for line in lines:
    match = re.search(pattern, line)
    if match:
        time_str = match.group(1)
        txn_number = int(match.group(2))
        duration = float(match.group(3))
        
        # Parse time (format: HH:MM:SS.mmm)
        time_obj = datetime.strptime(f"2026-01-13 {time_str}", "%Y-%m-%d %H:%M:%S.%f")
        transactions.append({
            'number': txn_number,
            'completion_time': time_obj,
            'duration': duration
        })

# Calculate overhead (time between completion of one transaction and completion of next)
for i in range(len(transactions) - 1):
    time_diff = (transactions[i+1]['completion_time'] - transactions[i]['completion_time']).total_seconds()
    transactions[i]['overhead'] = time_diff - transactions[i+1]['duration']

# Last transaction has no overhead to calculate
if transactions:
    transactions[-1]['overhead'] = 0

# Create visualization
fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 10))
fig.suptitle('Transaction Analysis', fontsize=16, fontweight='bold')

# Plot 1: Transaction Duration
txn_numbers = [t['number'] for t in transactions]
durations = [t['duration'] for t in transactions]
overheads = [t['overhead'] for t in transactions]

ax1.bar(txn_numbers, durations, color='steelblue', alpha=0.7, label='Transaction Duration')
ax1.set_xlabel('Transaction Number')
ax1.set_ylabel('Duration (seconds)')
ax1.set_title('Transaction Duration Over Time')
ax1.grid(True, alpha=0.3)
ax1.legend()

# Plot 2: Overhead
ax2.bar(txn_numbers[:-1], overheads[:-1], color='coral', alpha=0.7, label='Overhead')
ax2.set_xlabel('Transaction Number')
ax2.set_ylabel('Overhead (seconds)')
ax2.set_title('Overhead Between Transactions (Time from completion to next completion minus next duration)')
ax2.grid(True, alpha=0.3)
ax2.legend()

# Plot 3: Stacked view of Duration + Overhead
ax3.bar(txn_numbers[:-1], durations[:-1], color='steelblue', alpha=0.7, label='Transaction Duration')
ax3.bar(txn_numbers[:-1], overheads[:-1], bottom=durations[:-1], color='coral', alpha=0.7, label='Overhead')
ax3.set_xlabel('Transaction Number')
ax3.set_ylabel('Time (seconds)')
ax3.set_title('Combined View: Transaction Duration + Overhead')
ax3.grid(True, alpha=0.3)
ax3.legend()

plt.tight_layout()
plt.savefig('transaction_analysis.png', dpi=300, bbox_inches='tight')
print(f"Diagram saved as transaction_analysis.png")

# Print statistics
print(f"\nStatistics:")
print(f"Total transactions: {len(transactions)}")
print(f"Average duration: {sum(durations)/len(durations):.3f}s")
print(f"Average overhead: {sum(overheads[:-1])/(len(overheads)-1):.3f}s")
print(f"Min duration: {min(durations):.3f}s")
print(f"Max duration: {max(durations):.3f}s")
print(f"Min overhead: {min(overheads[:-1]):.3f}s")
print(f"Max overhead: {max(overheads[:-1]):.3f}s")
