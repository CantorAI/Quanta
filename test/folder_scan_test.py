# =============================================================
# vdb_simple_test.py — Simple folder scan, build VDB, query
# =============================================================

import os
import re
import torch
import clip
import numpy as np

import xlang
quanta = xlang.importModule("quanta", fromPath="Quanta")

# Load CLIP model
device = "cuda" if torch.cuda.is_available() else "cpu"
clip_model, _ = clip.load("ViT-B/32", device=device)
print(f"CLIP loaded on: {device}")


def extract_text_embedding(text):
    """Extract embedding, handles long text by chunking and averaging."""
    chunk_size = 200
    if len(text) <= chunk_size:
        text_input = clip.tokenize([text], truncate=True).to(device)
        with torch.no_grad():
            emb = clip_model.encode_text(text_input)
            emb = emb / emb.norm(dim=-1, keepdim=True)
        return emb.cpu().numpy().flatten()
    
    # Long text: chunk and average
    embeddings = []
    for i in range(0, len(text), chunk_size):
        chunk = text[i:i + chunk_size].strip()
        if chunk:
            text_input = clip.tokenize([chunk], truncate=True).to(device)
            with torch.no_grad():
                emb = clip_model.encode_text(text_input)
                emb = emb / emb.norm(dim=-1, keepdim=True)
                embeddings.append(emb.cpu().numpy().flatten())
    
    if not embeddings:
        return None
    avg = np.mean(embeddings, axis=0)
    return avg / np.linalg.norm(avg)


def scan_folder(folder_path, patterns):
    """Recursively scan folder for matching files."""
    result = []
    for entry in os.listdir(folder_path):
        full_path = os.path.join(folder_path, entry)
        if os.path.isdir(full_path):
            result += scan_folder(full_path, patterns)
        elif os.path.isfile(full_path):
            for p in patterns:
                if re.match(p, entry, re.IGNORECASE):
                    result.append(full_path)
                    break
    return result


# ============================================================
# CONFIG - Change these paths
# ============================================================
FOLDER_PATH = "D:/CantorAI/CantorModel/TestCode"
VDB_PATH = "D:/Test/simple_test.vdb"
PATTERNS = [r".*\.py$", r".*\.txt$", r".*\.md$", r".*\.x$"]

# ============================================================
# MAIN
# ============================================================
# Get embedding dimension
dim = extract_text_embedding("test").shape[0]
print(f"Embedding dim: {dim}")

# Scan files
files = scan_folder(FOLDER_PATH, PATTERNS)
print(f"Found {len(files)} files")

# Create VDB
vdb = quanta.vdb(dim, max_elements=len(files) * 100)

# Add files to VDB
id_counter = 0
for f in files:
    print(f"Adding: {f}")
    try:
        with open(f, 'r', encoding='utf-8', errors='ignore') as file:
            content = file.read()
        chunk = f"[{f}]\n{content[:200]}"
        emb = extract_text_embedding(chunk)
        if emb is not None:
            id_counter = vdb.AddVectors(id_counter, emb, chunks=chunk)
            id_counter += 1
    except Exception as e:
        print(f"  Error: {e}")

# Save
vdb.Save(VDB_PATH)
print(f"\nSaved VDB to {VDB_PATH}, total vectors: {id_counter}")

# Query
print("\n--- Query Test ---")
queries = ["vector database", "file processing", "scan folder"]
for q in queries:
    print(f"\nQuery: {q}")
    emb = extract_text_embedding(q)
    results = vdb.Lookup(emb, 3)
    print(f"Results: {results}")
