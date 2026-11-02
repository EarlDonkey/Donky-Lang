import json, time
raw = open("big.json", "rb").read()
print(f"File size: {len(raw)} bytes")
start = time.time()
for _ in range(100):
    doc = json.loads(raw)
elapsed_ms = (time.time() - start) * 1000
print(f"Python: {elapsed_ms:.1f}ms for 100 parses")