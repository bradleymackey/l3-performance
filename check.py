import numpy
import scipy.sparse
import scipy.sparse.linalg
import argparse


parser = argparse.ArgumentParser(description="""Check that the coded matrix multiplication is correct.

Only works on the binary file formats for matrices (see convert-to-binary.py for conversion).

Computes C = A B using scipy and then checks if ||C - actual|| / ||C|| is small.""")
parser.add_argument("matrices", metavar="M", nargs=2,
                    type=str,
                    help="Matrices to check multiplication of")
parser.add_argument("actual", type=str,
                    help="Result matrix to check")

args = parser.parse_args()


def read_matrix(filename):
    with open(filename, "rb") as f:
        mnz = numpy.fromfile(f, dtype=numpy.int32, count=3)
        nz = mnz[-1]
        coords = numpy.fromfile(f, dtype=numpy.int32, count=nz*2).reshape(nz, 2)
        v = numpy.fromfile(f, dtype=numpy.float64, count=nz)

    m, n, nz = mnz

    return scipy.sparse.coo_matrix((v, tuple(coords.T)), shape=(m, n))


print("multiply check READ start")
A, B = map(read_matrix, args.matrices)
print("multiply check READ stop")

actual = read_matrix(args.actual)


print("multiply check start")
expect = numpy.dot(A, B)
print("multiply check stop")

diff = expect - actual
print(diff)
rnorm = scipy.sparse.linalg.norm(diff) / scipy.sparse.linalg.norm(expect)

if rnorm < 1e-6:
    print("ok: ||E - A|| / ||E|| = {}".format(rnorm))
else:
    print("fail: ||E - A|| / ||E|| = {}".format(rnorm))
