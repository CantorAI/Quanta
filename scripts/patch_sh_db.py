import sqlite3
import os
import sys

def patch_db(db_path):
    print(f"Opening Database: {db_path}")
    if not os.path.exists(db_path):
        print("Error: Database file not found!")
        return

    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # 1. Extract necessary config items to compute max_elements
        cursor.execute("SELECT key, value FROM config WHERE key IN ('max_memory_gb', 'dimension', 'M')")
        rows = cursor.fetchall()
        config_map = {row[0]: row[1] for row in rows}
        
        # Verify the key doesn't already exist
        cursor.execute("SELECT value FROM config WHERE key = 'max_elements'")
        existing = cursor.fetchone()
        if existing:
            print(f"Success: Database already contains max_elements bound: {existing[0]}")
            return
            
        print(f"Extracted Config: {config_map}")
        
        max_mem_gb = float(config_map.get('max_memory_gb', 1.0))
        dimension = int(config_map.get('dimension', 512))
        m = int(config_map.get('M', 16))
        
        # 2. Replicate identical C++ math logic from Quanta VDB Engine
        bytes_per_vector = dimension * 4  # sizeof(float)
        graph_overhead = m * 4 * 2        # sizeof(int) * 2
        metadata_overhead = 256
        
        total_bytes_per_vector = bytes_per_vector + graph_overhead + metadata_overhead
        bytes_limit = int(max_mem_gb * 1024.0 * 1024.0 * 1024.0)
        
        max_elements = bytes_limit // total_bytes_per_vector
        if max_elements < 1000:
            max_elements = 1000
            
        print(f"\nComputed exact legacy limits:")
        print(f"- Total Bytes Per Vector: {total_bytes_per_vector}")
        print(f"- Max Elements Bound:     {max_elements}")
        
        # 3. Permanently INSERT mathematical bound into DB
        cursor.execute("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)", ('max_elements', str(max_elements)))
        conn.commit()
        print("\nSuccess: Inserted mathematical boundary into SQLite DB. The Quanta Engine will now safely hydrate this exact value on boot without destructive recalculations.")
        
    except Exception as e:
        print(f"Fatal error patching database: {e}")
    finally:
        if 'conn' in locals():
            conn.close()

if __name__ == "__main__":
    target = r"D:\CantorStorage-SH\vdb\clip_512\clip_config.db"
    patch_db(target)
