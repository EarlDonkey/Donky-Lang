import time

start = time.time()
i = 0
sum = 0.0

# 50 million iterations
while i < 50000000:
    sum += (i * 1.1)
    i += 1

end = time.time()
duration = end - start
print(f"Python Time: {duration:.4f} seconds")
print(f"Result: {sum}")