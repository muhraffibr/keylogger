import binascii
import os

def main():
    hex_file = "trace_hex.txt"
    bin_file = "trace.bin"
    
    if not os.path.exists(hex_file):
        print(f"[ERROR] '{hex_file}' not found.")
        print("Please create 'trace_hex.txt' and paste the hex string from the Serial Monitor inside it.")
        return
        
    try:
        with open(hex_file, "r") as f:
            hex_data = f.read().strip()
        
        # Remove any start/end headers if the user copied them too
        if "---START_TRACE_DUMP---" in hex_data:
            hex_data = hex_data.split("---START_TRACE_DUMP---")[1]
        if "---END_TRACE_DUMP---" in hex_data:
            hex_data = hex_data.split("---END_TRACE_DUMP---")[0]
            
        # Clean up all whitespaces/newlines
        hex_data = "".join(hex_data.split())
        
        # Convert hex to binary
        binary_data = binascii.unhexlify(hex_data)
        
        with open(bin_file, "wb") as f_out:
            f_out.write(binary_data)
            
        print(f"[SUCCESS] '{bin_file}' has been created successfully!")
        print("You can now open 'trace.bin' directly in Percepio Tracealyzer 4.")
    except Exception as e:
        print(f"[ERROR] Failed to convert hex to bin: {e}")

if __name__ == "__main__":
    main()
