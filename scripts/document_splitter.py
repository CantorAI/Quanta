import os
import json

# Cached optional dependencies
_PyPDF2 = None
_docx = None
_pd = None
_BeautifulSoup = None

# File extensions to chunk by lines (text/code files)
LINE_CHUNK_EXTENSIONS = (
    '.txt', '.c', '.cc', '.cpp', '.cxx', '.h',
    '.hh', '.hpp', '.hxx', '.js', '.ts', '.jsx', '.css',
    '.tsx', '.py', '.java', '.go', '.rs', '.swift',
    '.kt', '.x', '.md'
)

def load_pdf(file_path):
    global _PyPDF2
    if _PyPDF2 is None:
        try:
            import PyPDF2 as _tmp
            _PyPDF2 = _tmp
        except ImportError:
            raise ImportError('pip install PyPDF2 to load PDFs')
    text = ''
    with open(file_path, 'rb') as f:
        reader = _PyPDF2.PdfReader(f)
        for p in reader.pages:
            txt = p.extract_text()
            if txt:
                text += txt + '\n'
    return text

def load_docx(file_path):
    global _docx
    if _docx is None:
        try:
            import docx as _tmp
            _docx = _tmp
        except ImportError:
            raise ImportError('pip install python-docx to load DOCX')
    doc = _docx.Document(file_path)
    return '\n'.join(p.text for p in doc.paragraphs)

def load_excel(file_path):
    global _pd
    if _pd is None:
        try:
            import pandas as _tmp
            _pd = _tmp
        except ImportError:
            raise ImportError('pip install pandas openpyxl to load Excel')
    sheets = _pd.read_excel(file_path, sheet_name=None)
    out = []
    for name, df in sheets.items():
        out.append(f'--- Sheet: {name} ---')
        out.append(df.to_string())
    return '\n\n'.join(out)

def load_html(file_path):
    global _BeautifulSoup
    if _BeautifulSoup is None:
        try:
            from bs4 import BeautifulSoup as _tmp
            _BeautifulSoup = _tmp
        except ImportError:
            raise ImportError('pip install beautifulsoup4 to load HTML')
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()
    soup = _BeautifulSoup(content, 'html.parser')
    return soup.get_text(separator=' ', strip=True)

def load_txt(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        return f.read()

def load_json(file_path):
    with open(file_path, 'r', encoding='utf-8') as f:
        return json.dumps(json.load(f), indent=2)

def load_generic(file_path):
    return load_txt(file_path)

def load_document(file_path):
    """
    Read the contents of file_path into a single text string.
    """
    ext = os.path.splitext(file_path)[1].lower()
    if ext == '.pdf':
        return load_pdf(file_path)
    if ext == '.docx':
        return load_docx(file_path)
    if ext in ('.xls', '.xlsx'):
        return load_excel(file_path)
    if ext in ('.html', '.htm'):
        return load_html(file_path)
    if ext == '.json':
        return load_json(file_path)
    if ext in LINE_CHUNK_EXTENSIONS:
        return load_txt(file_path)
    return load_generic(file_path)


class TextChunker:
    def __init__(self, chunk_size=500, overlap=50):
        if chunk_size <= overlap:
            raise ValueError('chunk_size must be > overlap')
        self.chunk_size = chunk_size
        self.overlap = overlap

    def chunk(self, text):
        """
        Returns (chunks, metas):
          - chunks: list of text strings
          - metas:   list of dicts {'start': int, 'end': int}
        """
        chunks = []
        metas = []
        start = 0
        N = len(text)
        while start < N:
            end = start + self.chunk_size
            if end >= N:
                chunks.append(text[start:N])
                metas.append({'start': start, 'end': N})
                break
            chunks.append(text[start:end])
            metas.append({'start': start, 'end': end})
            start = end - self.overlap
        return chunks, metas


class CodeChunker:
    def __init__(self, lines_per_chunk=50, overlap_lines=5):
        if lines_per_chunk <= overlap_lines:
            raise ValueError('lines_per_chunk must be > overlap_lines')
        self.lines_per_chunk = lines_per_chunk
        self.overlap = overlap_lines

    def chunk(self, text):
        """
        Returns (chunks, metas):
          - chunks: list of code strings
          - metas:   list of dicts {'start_line': int, 'end_line': int}
        """
        lines = text.splitlines(keepends=True)
        chunks = []
        metas = []
        start = 0
        total = len(lines)
        while start < total:
            end = start + self.lines_per_chunk
            if end >= total:
                chunk_lines = lines[start:total]
                chunks.append(''.join(chunk_lines))
                metas.append({'start_line': start, 'end_line': total})
                break
            chunk_lines = lines[start:end]
            chunks.append(''.join(chunk_lines))
            metas.append({'start_line': start, 'end_line': end})
            start = end - self.overlap
        return chunks, metas


def split_document(file_path,
                   text_chunk_size=500,
                   text_overlap=50,
                   code_lines=5,
                   code_overlap=1):
    """
    Load file_path and split into chunks based on file type.
    Returns a list of chunk strings (ignores metadata).
    """
    text = load_document(file_path)
    ext = os.path.splitext(file_path)[1].lower()

    if ext in LINE_CHUNK_EXTENSIONS:
        chunker = CodeChunker(lines_per_chunk=code_lines,
                              overlap_lines=code_overlap)
    else:
        chunker = TextChunker(chunk_size=text_chunk_size,
                              overlap=text_overlap)

    chunks, _metas = chunker.chunk(text)
    return chunks


# Example usage
if __name__ == '__main__':
    for path in [
        'd:/test/xlang_spec.pdf',
        'D:/CantorAI2/Rockvault/DataFrame/DataFrameWrapper.cpp',
        'd:/test/CMakeLists.txt',
    ]:
        print(f'\n--- Chunks for {path} ---')
        try:
            for chunk in split_document(path):
                preview = chunk[:40].replace('\n', '\\n')
                print(f'[{preview}...]')
        except Exception as e:
            print(f'  ERROR: {e}')
