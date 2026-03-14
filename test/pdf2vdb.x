import cantor thru 'lrpc:1000'
import pdf_embedding
from Quanta import quanta
print("Init")
# --- Configuration ---
pdf_path = "d:/Test/xlang_spec.pdf"
vdb_path = "d:/Test/test104.vdb"
embed_dim = 768
capacity = 200

# Initialize Cantor/Quanta and VDB
quanta.cantor = cantor
vdb = quanta.vdb(embed_dim, max_elements = capacity,space="l2")

# Build and save the VDB from PDF
def build_vdb():
    py_text = pdf_embedding.extract_text(pdf_path)
    text = to_xlang(py_text)
    py_chunks = pdf_embedding.chunk_text(text)
    chunks = to_xlang(py_chunks)
    py_embeddings = pdf_embedding.get_embeddings(chunks)
    embeddings = to_xlang(py_embeddings)
    vdb.AddVectors(0,embeddings,chunks = chunks)
    vdb.Save(vdb_path)
    print("VDB saved to", vdb_path)

# Query the VDB with an input text
def query_text(input_text, top_k):
    py_q_emb = pdf_embedding.embed_text(input_text)
    q_emb = to_xlang(py_q_emb)
    return vdb.Lookup(q_emb, top_k)

# Main entry point for XLang script
def main():
    print("Building VDB from PDF...")
    #build_vdb()
    vdb.Load("d:/Test/test104.vdb")

    example_query3 = "Launch debugging from a file."
    print("Querying for:", example_query3)
    results3 = query_text(example_query3, 1)
    print("Top results for 3:", results3)

    # Example query
    example_query1 = "XLang is a Python-like programming language"
    print("Querying for:", example_query1)
    results1 = query_text(example_query1, 1)
    print("Top results:", results1)
    # vdb.Load("d:/Test/test101.vdb")
    example_query2 = "Classes are also first-class objects"
    print("Querying for:", example_query2)
    results2 = query_text(example_query2, 1)
    print("Top results for 2:", results2)


# Invoke main
main()
