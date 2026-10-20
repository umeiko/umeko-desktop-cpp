#include "syntaxhighlighter.hpp"

struct RuleHighlighter : QSyntaxHighlighter {
  using QSyntaxHighlighter::QSyntaxHighlighter;
  QString language;
  bool dark = false, valid = false;
  void highlightBlock(const QString &text) override {
    if (!valid || text.size() > 2000 || document()->characterCount() > 48001 ||
        document()->blockCount() > 1000)
      return;
    auto color = [&](QString light, QString dim) {
      QTextCharFormat format;
      format.setForeground(QColor(dark ? dim : light));
      return format;
    };
    auto paint = [&](QString expression, QTextCharFormat format) {
      auto it = QRegularExpression(expression).globalMatch(text);
      int count = 0;
      while (it.hasNext() && ++count < 2000) {
        auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), format);
      }
    };
    if (language == "diff") {
      if (text.startsWith('+'))
        setFormat(0, text.size(), color("#26784b", "#a8cf9c"));
      else if (text.startsWith('-'))
        setFormat(0, text.size(), color("#b14444", "#ef9a9a"));
      return;
    }
    paint(
        "\\b(?:if|else|elif|for|while|return|class|def|function|const|let|var|"
        "export|default|import|from|try|except|catch|throw|new|public|private|"
        "static|void|int|float|double|bool|char|auto|async|await|yield|with|as|"
        "in|not|and|or|pass|break|continue|switch|case|interface|struct|enum|"
        "package|using|namespace|fn|mut|use|impl|match|select|where|SELECT|"
        "FROM|WHERE|INSERT|UPDATE|CREATE|true|false|True|False|null|None)\\b",
        color("#963b83", "#c99bdc"));
    paint("\\b(?:0x[\\da-fA-F]+|\\d+(?:\\.\\d+)?)\\b",
          color("#a15d17", "#e5b281"));
    paint("\\b[A-Za-z_]\\w*(?=\\s*\\()", color("#2b65ad", "#8cbce9"));
    paint("(?:\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'|`(?:[^`\\\\]|\\\\."
          ")*`)",
          color("#26784b", "#a8cf9c"));
    bool hash =
        QStringList{"python", "bash", "shell", "yaml", "powershell"}.contains(
            language);
    paint(hash ? "#[^\\n]*" : "//[^\\n]*", color("#7b827b", "#87978c"));
    if (QStringList{"html", "xml"}.contains(language))
      paint("</?[A-Za-z][^>]*>", color("#963b83", "#c99bdc"));
  }
};
SyntaxHighlighter::SyntaxHighlighter(QObject *parent) : QObject(parent) {
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(120);
  connect(&m_debounce, &QTimer::timeout, this, &SyntaxHighlighter::apply);
}
void SyntaxHighlighter::setDocument(QQuickTextDocument *doc) {
  if (m_document == doc)
    return;
  if (m_document)
    disconnect(m_document->textDocument(), nullptr, this, nullptr);
  delete m_highlighter;
  m_document = doc;
  m_highlighter = nullptr;
  if (doc) {
    m_highlighter = new RuleHighlighter(doc->textDocument());
    connect(doc->textDocument(), &QTextDocument::contentsChange, this,
            [this](int, int removed, int added) {
              if (removed || added)
                m_debounce.start();
            });
  }
  m_debounce.start();
  emit changed();
}
void SyntaxHighlighter::setLanguage(QString value) {
  if (m_language == value)
    return;
  m_language = value;
  m_debounce.start();
  emit changed();
}
void SyntaxHighlighter::setDark(bool value) {
  if (m_dark == value)
    return;
  m_dark = value;
  apply();
  emit changed();
}
void SyntaxHighlighter::setEnabled(bool value) {
  if (m_enabled == value)
    return;
  m_enabled = value;
  apply();
  emit changed();
}
void SyntaxHighlighter::apply() {
  if (!m_document || !m_highlighter)
    return;
  auto h = static_cast<RuleHighlighter *>(m_highlighter);
  auto doc = m_document->textDocument();
  auto language = m_language.toLower();
  static const QHash<QString, QString> aliases = {
      {"py", "python"},      {"python3", "python"}, {"js", "javascript"},
      {"jsx", "javascript"}, {"ts", "typescript"},  {"tsx", "typescript"},
      {"sh", "bash"},        {"ps1", "powershell"}, {"yml", "yaml"},
      {"c++", "cpp"},        {"cc", "cpp"},         {"rs", "rust"},
      {"golang", "go"},      {"htm", "html"},       {"cs", "csharp"}};
  language = aliases.value(language, language);
  static const QSet<QString> supported = {
      "python",     "javascript", "typescript", "json",   "yaml", "bash",
      "powershell", "c",          "cpp",        "csharp", "java", "go",
      "rust",       "sql",        "html",       "xml",    "css",  "diff"};
  bool eligible = m_enabled && supported.contains(language) &&
                  doc->characterCount() <= 48001 && doc->blockCount() <= 1000;
  for (auto block = doc->begin(); eligible && block.isValid();
       block = block.next())
    if (block.length() > 2001)
      eligible = false;
  const bool previouslyColored = h->valid;
  h->valid = eligible;
  h->language = language;
  h->dark = m_dark;
  if (eligible || previouslyColored)
    h->rehighlight();
  if (m_colored != eligible) {
    m_colored = eligible;
    emit changed();
  }
}
