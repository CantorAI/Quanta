# document_splitter.py

import os
import json

# --- optional dependencies
try:
    import PyPDF2
except ImportError:
    PyPDF2 = None

try:
    import docx
except ImportError:
    docx = None

try:
    import pandas as pd
except ImportError:
    pd = None

try:
    from bs4 import BeautifulSoup
except ImportError:
    BeautifulSoup = None

# File extensions to chunk by lines (text/code files)
LINE_CHUNK_EXTENSIONS = (".txt", ".c", ".cc", ".cpp", ".cxx", ".h", 
                         ".hh", ".hpp", ".hxx", ".js", ".ts", ".jsx", ".css",
                         ".tsx", ".py", ".java", ".go", ".rs", ".swift", 
                         ".kt", ".x",".md")

def load_pdf(file_path):
    if PyPDF2 is None:
        raise ImportError("pip install PyPDF2 to load PDFs")
    text = ""
    with open(file_path, "rb") as f:
        reader = PyPDF2.PdfReader(f)
        for p in reader.pages:
            txt = p.extract_text()
            if txt:
                text += txt + "\n"
    return text


def load_docx(file_path):
    if docx is None:
        raise ImportError("pip install python-docx to load DOCX")
    doc = docx.Document(file_path)
    return "\n".join(p.text for p in doc.paragraphs)


def load_excel(file_path):
    if pd is None:
        raise ImportError("pip install pandas openpyxl to load Excel")
    sheets = pd.read_excel(file_path, sheet_name=None)
    out = []
    for name, df in sheets.items():
        out.append(f"--- Sheet: {name} ---")
        out.append(df.to_string())
    return "\n\n".join(out)


def load_html(file_path):
    if BeautifulSoup is None:
        raise ImportError("pip install beautifulsoup4 to load HTML")
    with open(file_path, "r", encoding="utf-8") as f:
        soup = BeautifulSoup(f, "html.parser")
    return soup.get_text(separator=" ", strip=True)


def load_txt(file_path):
    with open(file_path, "r", encoding="utf-8") as f:
        return f.read()


def load_json(file_path):
    with open(file_path, "r", encoding="utf-8") as f:
        return json.dumps(json.load(f), indent=2)


def load_generic(file_path):
    return load_txt(file_path)


def load_document(file_path):
    """
    Read the contents of file_path into a single text string.
    """
    ext = os.path.splitext(file_path)[1].lower()
    if ext == ".pdf":
        return load_pdf(file_path)
    if ext == ".docx":
        return load_docx(file_path)
    if ext in (".xls", ".xlsx"):
        return load_excel(file_path)
    if ext in (".html", ".htm"):
        return load_html(file_path)
    if ext == ".json":
        return load_json(file_path)
    if ext in LINE_CHUNK_EXTENSIONS:
        return load_txt(file_path)
    return load_generic(file_path)


class TextChunker:
    def __init__(self, chunk_size=500, overlap=50):
        if chunk_size <= overlap:
            raise ValueError("chunk_size must be > overlap")
        self.chunk_size = chunk_size
        self.overlap = overlap

    def chunk(self, text):
        """
        Returns list of {chunk, start, end}, where
        start/end are character offsets in `text`.
        """
        chunks = []
        start = 0
        N = len(text)
        while start < N:
            end = start + self.chunk_size
            if end >= N:
                chunks.append(text[start:N])# chunks.append({"chunk": text[start:N], "start": start, "end": N})
                break
            chunks.append(text[start:end]) #chunks.append({"chunk": text[start:end], "start": start, "end": end})
            start = end - self.overlap
        return chunks


class CodeChunker:
    def __init__(self, lines_per_chunk=50, overlap_lines=5):
        if lines_per_chunk <= overlap_lines:
            raise ValueError("lines_per_chunk must be > overlap_lines")
        self.lines_per_chunk = lines_per_chunk
        self.overlap = overlap_lines

    def chunk(self, text):
        lines = text.splitlines(keepends=True)
        chunks = []
        start = 0
        total = len(lines)
        while start < total:
            end = start + self.lines_per_chunk
            if end >= total:
                chunk_lines = lines[start:total]
                chunks.append({
                    "chunk": "".join(chunk_lines),
                    "start_line": start,
                    "end_line": total
                })
                break
            chunk_lines = lines[start:end]
            chunks.append({
                "chunk": "".join(chunk_lines),
                "start_line": start,
                "end_line": end
            })
            start = end - self.overlap
        return chunks


def split_document(file_path,
                   text_chunk_size=500,
                   text_overlap=50,
                   code_lines=50,
                   code_overlap=5):
    """
    Load `file_path` and split into chunks based on file type.
    Returns a list of dicts with keys depending on the chunker:
      - text: {'chunk','start','end'}
      - code: {'chunk','start_line','end_line'}
    """
    text = load_document(file_path)
    ext = os.path.splitext(file_path)[1].lower()

    if ext in LINE_CHUNK_EXTENSIONS:
        chunker = CodeChunker(lines_per_chunk=code_lines,
                              overlap_lines=code_overlap)
    else:
        chunker = TextChunker(chunk_size=text_chunk_size,
                               overlap=text_overlap)

    return chunker.chunk(text)


# Example
if __name__ == "__main__":
    for path in [
        "d:/test/xlang_spec.pdf",
        "d:/test/value.h",
        "d:/test/CMakeLists.txt",
    ]:
        print(f"\n--- Chunks for {path} ---")
        try:
            for info in split_document(path):
                preview = info["chunk"][:40].replace("\n", "\\n")
                loc = (
                    f"{info.get('start')}--{info.get('end')}"
                    if "start" in info else f"lines {info['start_line']}--{info['end_line']}"
                )
                print(f"[{loc}] {preview}...")
        except Exception as e:
            print(f"  ERROR: {e}")
