# folder2vdb.x

import cantor thru "lrpc:1000"
import document_splitter
import embedding_api
from xlang_os import fs
from Quanta import quanta

quanta.cantor = cantor

def matches_filter(file_name, regex_patterns):
        for regex in regex_patterns:
            match = file_name.regex_match(regex)
            if match:
                return True
        return False

def scan_folder_recursive(folder_path, regex_patterns):
    folder    = fs.Folder(folder_path)
    all_files = folder.Scan()
    result    = []

    for entry in all_files:
        name      = entry["Name"]
        full_path = folder.BuildPath(name)
        if entry["IsDirectory"] == "true":
            result += scan_folder_recursive(full_path, regex_patterns)
        elif matches_filter(name, regex_patterns):
            result.append(full_path)

    return result

def build_vdb_from_folder(root_folder, patterns, model_name="mpnet", index_path="vdb.index"):
    # 1) init embedder
    embedder = embedding_api.EmbeddingAPI()
    embedder.set_active_model(model_name)

    # 2) scan files
    files = scan_folder_recursive(root_folder, patterns)
    print("Found ${files.size()} files to process.")

    # 3) determine dim
    dim = embedder.embed_text("test").to_xlang().size()

    # 4) init Quanta Vdb
    max_elems = files.size() * 200
    vdb = quanta.vdb(dim, max_elements = max_elems)

    # 5) ingest
    id_counter = 0
    for path in files:
        print("File:",path)
        py_chunks = document_splitter.split_document(path)
        chunks = to_xlang(py_chunks)
        py_embs   = embedder.embed_chunks(chunks)
        embs = to_xlang(py_embs)
        id_counter = vdb.AddVectors(id_counter,embs,chunks = chunks)
        id_counter +=1

    # 6) save
    vdb.Save(index_path)
    print("Saved VDB to {}".format(index_path))

filter = ".*\.txt$;.*\.c$;.*\.cc$;.*\.cpp$;.*\.cxx$;.*\.h$;.*\.hh$;.*\.hpp$;.*\.hxx$;.*\.js$;.*\.ts$;.*\.jsx$;.*\.css$;.*\.tsx$;.*\.py$;.*\.java$;.*\.go$;.*\.rs$;.*\.swift$;.*\.kt$;.*\.x$;.*\.md$"
regex_patterns = filter.split(";")
build_vdb_from_folder("D:\\Test", regex_patterns,"mpnet","d:/Test/test104.vdb")
print("Done")
