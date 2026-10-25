from . import square
from . import mathutils

values = []
for i in range(5):
    values.append(square(i))
print(values)
print(mathutils.square(12))
