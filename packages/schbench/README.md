# Schbench

Benchpress includes Linux kernel's schbench tool.

Schbench can be invoked using benchpress cli:

```
./benchpress_cli.py -b system run schbench_default
```

Or, with custom runtime:
```
./benchpress_cli.py -b system run schbench_custom -i '{"runtime":20}'
```
