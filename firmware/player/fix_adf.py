import os, re, sys

base = r'C:\Users\default.DESKTOP-FC9SLHI\source\repos\esp32-music-player\firmware\player\esp-adf\components'
skip_dirs = {'test', 'test_apps', 'examples'}
pats = [
    (r'\bxQueueHandle\b',    'QueueHandle_t'),
    (r'\bxTaskHandle\b',     'TaskHandle_t'),
    (r'\bportTickType\b',    'TickType_t'),
    (r'\bportBASE_TYPE\b',   'BaseType_t'),
    (r'\bxSemaphoreHandle\b','SemaphoreHandle_t'),
    (r'\bxTimerHandle\b',    'TimerHandle_t'),
    (r'\bportTICK_RATE_MS\b','portTICK_PERIOD_MS'),
]

changed = 0
for root, dirs, files in os.walk(base):
    dirs[:] = [d for d in dirs if d not in skip_dirs]
    for fname in files:
        if not (fname.endswith('.c') or fname.endswith('.h')):
            continue
        path = os.path.join(root, fname)
        try:
            with open(path, encoding='utf-8', errors='replace') as fh:
                content = fh.read()
            new = content
            for pat, rep in pats:
                new = re.sub(pat, rep, new)
            if new != content:
                with open(path, 'w', encoding='utf-8') as fh:
                    fh.write(new)
                print('Fixed:', os.path.relpath(path, base))
                changed += 1
        except Exception as e:
            print('ERR', path, e, file=sys.stderr)

print(f'\nTotal: {changed} files patched')
