# Data

A csv table:

```csv
a,b
1,"x, y"
2,z
```

A json table:

```json
[{"a": 1, "b": "x"}, {"a": 2, "c": true}]
```

A broken csv:

```csv
a,b
1,"unterminated
```

Plain code stays code:

```python
print(1)
```
