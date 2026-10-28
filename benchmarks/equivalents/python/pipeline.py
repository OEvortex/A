data = [n for n in range(0, 5000)]
total = sum(n * 2 for n in data if n % 3 == 0)
print(total)
