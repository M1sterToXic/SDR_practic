import numpy as np
import matplotlib.pyplot as plt

name = "build/sdr1_rxdata.pcm"

data = np.fromfile(name, dtype=np.int16)

real = data[0::2]
imag = data[1::2]

plt.figure(1, figsize=(8, 8))
plt.plot(real, imag, '.', color='blue', markersize=1)

plt.title("I/Q Diagram")
plt.xlabel("I (Real)")
plt.ylabel("Q (Imaginary)")
plt.grid(True)
plt.axis('equal')
plt.show()