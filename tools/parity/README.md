# Differential check against the original C# library

`tests/data/expected_queries.txt` is produced by the C# implementation and checked by
`fh_tests`: every CSS selector of `CssTests` and every XPath expression of `XPathTests`
must give the same ids in the same order.

To regenerate:

```sh
git clone --depth 1 https://github.com/romanti3172/z3l.FastHtmlParser ref/csharp
dotnet run -c Release --project tools/parity -- tests/data > tests/data/expected_queries.txt
./build/fh_tests fixtures tests/data
```
