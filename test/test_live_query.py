import sys
import os

# 1. Bind Cantor & Quanta binaries for IPC RPC
xlang_bin_dir = "D:\\CantorAI\\out\\build\\x64-Debug\\bin"
os.environ["PATH"] += os.pathsep + xlang_bin_dir
sys.path.append(xlang_bin_dir)

import xlang3

# 2. Add CantorModel to path for CLIP Embedding Extraction
vision_model_dir = "D:\\CantorAI\\CantorModel\\VisionDetect"
sys.path.append(vision_model_dir)

import numpy as np
import time
import clip

def main():
    print("=========================================================================")
    print("=== LIVE Cantor VDB Query Test (CLIP 512) ===")
    print("=========================================================================")
    
    # ---------------------------------------------------------
    # Connect to live CANTOR Engine singleton over RPC
    # ---------------------------------------------------------
    print("\n[+] Routing Cantor RPC over lrpc:1000...")
    try:
        # Standard LRPC connection natively bypassing process lock limits
        cantor = xlang3.importModule("cantor", thru="lrpc:1000")
        quanta = cantor.QueryModule("quanta")
    except Exception as e:
        print(f"[-] FAILED to bind to running Cantor RPC node! Ensure CantorAI is active. {e}")
        sys.exit(1)
        
    print("[+] Successfully bound to live Cantor execution engine.")

    # ---------------------------------------------------------
    # Mount Live VDB Target natively via Quanta
    # ---------------------------------------------------------
    print(f"\n[+] Requesting live active Quanta VDB graph from Cantor...")
    try:
        db_path = "D:\\CantorStorage-SH\\vdb\\clip_512"
        print(f"  -> dynamically resolved path: {db_path}")

        # Mount the engine-hosted VDB explicitly by path inside Cantor's process natively
        vdb = quanta.GetPartitionedVdb(
            prefix="clip",
            path=db_path,
            dim=512,
            granularity="hourly"
        )
    except Exception as e:
        print(f"[-] FAILED to mount active VDB partition! {e}")
        sys.exit(1)
        
    print(f"[+] Successfully hooked Database Graph natively. Generating payload...")
    
    # ---------------------------------------------------------
    # Generate CLIP 512 Text Embeddings and Query
    # ---------------------------------------------------------
    queries = ["red car", "person walking", "blue truck", "dog", "night time street"]
    print(f"\n[+] Initializing OpenAI CLIP backend locally for Tokenization...")
    
    import torch
    device = "cuda" if torch.cuda.is_available() else "cpu"
    
    try:
        model, preprocess = clip.load("ViT-B/32", device=device)
        print(f"[+] CLIP Model loaded precisely on {device}")
        
        for loop_idx in range(1000):
            print(f"\n==========================================")
            print(f"=== Benchmark Round {loop_idx + 1}/1000 ===")
            print(f"==========================================")
            
            for search_text in queries:
                print(f"\n[*] Querying string: '{search_text}'")
                
                # Tokenize and execute model encode layer natively
                text_input = clip.tokenize([search_text]).to(device)
                with torch.no_grad():
                    text_features = model.encode_text(text_input)
                    
                # Natively cast back to flat Python lists (FP32)
                text_features = text_features.cpu().numpy().astype(np.float32)
                query_np = text_features
                
                t0 = time.time()
                
                # Empty partition uses the global index graph sweeping history
                results = vdb.Lookup(query_np, 5, dedup=0.5, ts_start=0) 
                
                t1 = time.time()
                latency = (t1 - t0) * 1000
                
                print(f"  -> Search executed in {latency:.2f}ms. Matching Nodes Found: {len(results) if results else 0}")
                
                if results:
                    for i, match in enumerate(results):
                        # match: [ExtID, Score, MetaStruct, Key, Timestamp]
                        print(f"    Match #{i+1} : id={match[0]}, score={match[1]:.4f}, key='{match[3]}', ts={match[4]}")
                else:
                    print("    [-] No valid targets found.")
            
            print(f"\n[+] Sleeping for 5 seconds before next round...")
            time.sleep(0.1)
                
    except Exception as e:
        print(f"[-] FATAL ERROR executing CLIP Model: {e}")
        sys.exit(1)

    print("\n[+] Gracefully terminating script handles.")
    vdb.Close()
    
if __name__ == "__main__":
    main()
