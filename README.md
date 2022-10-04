# pngshrink

Simple program that uses libpng to progressively read and write shrunken pngs

Used for learning about coroutines, PNGs, etc. and can be further developed.

How to use:
```
./pngshrink inFile outFile sampleRate
```
i.e.
```
./pngshrink palm-tree.png palm-tree-mini.png 3
```
Will create a smaller (1/3 size) valid png image file of a palm tree
