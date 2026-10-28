import math

acc = 0.0
for i in range(1, 50000):
    acc += math.sqrt(float(i))
print(round(acc))
