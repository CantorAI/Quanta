# File: pdf_embedding.py (Python module)
# --- Dependencies: PyPDF2, sentence-transformers ---
import sys
import PyPDF2
from sentence_transformers import SentenceTransformer
import numpy as np
from numpy.linalg import norm

# Embedding model & text-chunk settings
# enable_model = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
enable_model = SentenceTransformer("all-mpnet-base-v2")

chunk_size = 100
overlap = 20

# Extract full text from a PDF file
def extract_text(path):
    reader = PyPDF2.PdfReader(path)
    text = ""
    for page in reader.pages:
        txt = page.extract_text()
        if txt:
            text += txt + "\n"
    return text

# Split text into overlapping chunks
def chunk_text(text):
    paragraphs = text.split("\n")
    chunks = [p for p in paragraphs if len(p)>50]
    return chunks

def chunk_text2(text):
    chunks = []
    start = 0
    length = len(text)
    while start < length:
        end = start + chunk_size
        if end > length:
            end = length
        chunks.append(text[start:end])
        start += chunk_size - overlap
    return chunks

# Compute embeddings for a list of text chunks
def get_embeddings(chunks):
    embeddings = enable_model.encode(
        chunks,
        show_progress_bar=True,
        convert_to_numpy=True
    )
    # pick a random pair of chunks
    i, j = 6,1
    d = norm(embeddings[i] - embeddings[j])
    print("Euclidean distance between chunk 0 and 1:", d)
    print("Min/max distances:", np.min(norm(embeddings - embeddings[:,None], axis=2)),
                                np.max(norm(embeddings - embeddings[:,None], axis=2)))
    return embeddings

# Compute embedding for a single text query
def embed_text(text):
    emb = enable_model.encode(
        [text],
        convert_to_numpy=True
    )[0]
    return emb

# --- Test / Demo section ---
if __name__ == "__main__":
    pdf_path = "xlang_spec.pdf"
    print(f"Loading PDF: {pdf_path}")
    full_text = extract_text(pdf_path)
    print(f"Extracted text length: {len(full_text)} characters")

    chunks = chunk_text(full_text)
    print(f"Split into {len(chunks)} chunks (size={chunk_size}, overlap={overlap})")

    embeddings = get_embeddings(chunks)
    print(f"Computed embeddings shape: {embeddings.shape}")
    print(f"First embedding vector (first 10 dims): {embeddings[0][:10]}")

    query = "Syntax is inspired by C++ with"
    q_emb = embed_text(query)
    print(f"Query embedding (first 10 dims): {q_emb[:10]}")
