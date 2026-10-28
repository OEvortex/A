total = 0
for _ in range(0, 500):
    parts = [str(n) for n in range(0, 200)]
    total += len(",".join(parts))
print(total)
