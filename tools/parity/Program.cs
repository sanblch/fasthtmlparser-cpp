using z3l.FastHtmlParser;
using z3l.FastHtmlParser.Shortcuts;

static string Esc(string? s) => s == null ? "<null>" : s.Replace("\\", "\\\\").Replace("\n", "\\n").Replace("\r", "\\r").Replace("\t", "\\t");
static string Ids(IEnumerable<ParserTag> tags) => string.Join(",", tags.Select(t => t.AttributeByName(HtmlAttribute.Id)));

var dir = args[0];
var cssPage = File.ReadAllText(Path.Combine(dir, "css_page.html"));
var xpathPage = File.ReadAllText(Path.Combine(dir, "xpath_page.html"));
var cssDoc = new HtmlParser(cssPage);
var xpathDoc = new HtmlParser(xpathPage);

foreach (var line in File.ReadAllLines(Path.Combine(dir, "css_selectors.txt")))
{
    if (line.Length == 0) continue;
    try { Console.WriteLine($"CSS\t{line}\t{Ids(cssDoc.Css(line))}"); }
    catch (Exception e) { Console.WriteLine($"CSS\t{line}\tERROR"); }
}

foreach (var line in File.ReadAllLines(Path.Combine(dir, "xpath_expressions.txt")))
{
    if (line.Length == 0) continue;
    try { Console.WriteLine($"XP\t{line}\t{Ids(xpathDoc.XPath(line))}"); }
    catch (Exception e) { Console.WriteLine($"XP\t{line}\tERROR"); }
}

foreach (var line in File.ReadAllLines(Path.Combine(dir, "xpath_values.txt")))
{
    if (line.Length == 0) continue;
    try { Console.WriteLine($"VAL\t{line}\t{Esc(xpathDoc.XPathValue(line))}"); }
    catch (Exception e) { Console.WriteLine($"VAL\t{line}\tERROR"); }
}
