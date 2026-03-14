# build_and_query_vdb.py
# ----------------------------------------
# Dependencies: PyPDF2, sentence-transformers, numpy, hnswlib
# Install via:
#   pip install PyPDF2 sentence-transformers numpy hnswlib

import os
import numpy as np
import hnswlib
import pdf_embedding  # your module

# --- User parameters ---
PDF_PATH   = r"d:/Test/xlang_spec.pdf"
INDEX_PATH = r"d:/Test/test_vdb_hnswlib3.bin"
EMBED_DIM  = 768       # must match your model output
TOP_K      = 3         # how many neighbors to return

# --- Step 1: Extract & chunk the PDF ---
print(f"Loading PDF from {PDF_PATH} ...")
text   = pdf_embedding.extract_text(PDF_PATH)
chunks = pdf_embedding.chunk_text(text)
print(f"Generated {len(chunks)} chunks.")

# --- Step 2: Compute embeddings ---
print("Computing embeddings (this may take a while)...")
embeddings = pdf_embedding.get_embeddings(chunks)  # shape: (n_chunks, EMBED_DIM)
if embeddings.shape[1] != EMBED_DIM:
    raise ValueError(f"Embedding dimension mismatch: expected {EMBED_DIM}, got {embeddings.shape[1]}")

# --- Step 3: Build or load HNSW index ---
# p = hnswlib.Index(space='cosine', dim=EMBED_DIM)
p = hnswlib.Index(space='l2', dim=EMBED_DIM)

if os.path.exists(INDEX_PATH):
    print(f"Loading existing index from {INDEX_PATH} ...")
    p.load_index(INDEX_PATH)
else:
    print("Initializing new HNSW index...")
    p.init_index(max_elements=len(chunks), ef_construction=200, M=16)
    print("Adding items to index...")
    p.add_items(embeddings, np.arange(len(chunks)))
    p.set_ef(50)  # query-time accuracy/speed tradeoff
    print(f"Saving index to {INDEX_PATH} ...")
    p.save_index(INDEX_PATH)

# --- Step 4: Query the index ---
def query_vdb(query_str):
    print(f"\nQuerying for: “{query_str}”")
    q_emb     = pdf_embedding.embed_text(query_str)
    labels, distances = p.knn_query(q_emb, k=TOP_K)
    for rank, (label, dist) in enumerate(zip(labels[0], distances[0]), start=1):
        snippet = chunks[label].replace("\n", " ").strip()[:200]
        print(f"  [{rank}] chunk #{label} (dist={dist:.4f}): {snippet}…")

# Example queries
query_strings = [
    "Launch debugging from a file.",
    "XLang is a Python-like programming language",
    "Classes are also first-class objects"
]

for qs in query_strings:
    query_vdb(qs)

print("Done")
