import binascii
import os
import re

def clean_hex_data(raw_data):
    # Split the raw data into lines
    lines = raw_data.splitlines()
    cleaned_lines = []
    
    for line in lines:
        line_str = line.strip()
        if not line_str:
            continue
            
        # Ignore warning lines, stack monitors, or divider lines
        if "PHYSICAL TAMPERING" in line_str or "MONITOR STACK" in line_str:
            print(f"[CLEAN] Removing log line: {line_str}")
            continue
        if re.match(r'^[-=#\s]*$', line_str):
            print(f"[CLEAN] Removing divider line: {line_str}")
            continue
            
        # Let's clean up interleaved logs inside the hex line
        # Check for [LOG] Plaintext Suhu:
        if "[LOG]" in line_str:
            print(f"[CLEAN] Found interleaved log in line: {line_str[:50]}...")
            
            # Use regex to find and remove:
            # 1) '[LOG] Plaintext Suhu: '
            # 2) '<number> C | Ciphertext (Enkripsi): <hex bytes>'
            # First, strip the prefix tag
            line_str = line_str.replace("[LOG] Plaintext Suhu: ", "")
            
            # Now find and remove the trailing suffix pattern: e.g. "0.00 C | Ciphertext (Enkripsi): 7B 65 7B 7B"
            # The suffix can be e.g. "0.00 C | Ciphertext (Enkripsi): 7B 65 7B 7B" or ".00 C | Ciphertext (Enkripsi): 7B 65 7B 7B"
            suffix_regex = re.compile(r'[0-9.]*\s*C\s*\|\s*Ciphertext\s*\(Enkripsi\):\s*[0-9A-Fa-f\s]+')
            match = suffix_regex.search(line_str)
            if match:
                print(f"[CLEAN] Removing suffix: {match.group(0)}")
                line_str = suffix_regex.sub("", line_str)
        
        # Clean whitespaces/newlines from the line
        line_str = "".join(line_str.split())
        
        # Keep only hex characters
        hex_only = re.sub(r'[^0-9a-fA-F]', '', line_str)
        if len(hex_only) != len(line_str):
            print(f"[CLEAN] Warning: Removed non-hex characters: '{re.sub(r'[0-9a-fA-F]', '', line_str)}'")
            
        cleaned_lines.append(hex_only)
        
    return "".join(cleaned_lines)

def main():
    hex_file = "trace_hex.txt"
    bin_file = "trace.bin"
    
    if not os.path.exists(hex_file):
        # Check in parent directory
        if os.path.exists(os.path.join("..", hex_file)):
            hex_file = os.path.join("..", hex_file)
        else:
            print(f"[ERROR] '{hex_file}' not found.")
            return
            
    print(f"[INFO] Reading from '{hex_file}'...")
    with open(hex_file, "r") as f:
        raw_data = f.read()
        
    cleaned_hex = clean_hex_data(raw_data)
    
    # Check length
    if len(cleaned_hex) % 2 != 0:
        print(f"[WARNING] Cleaned hex length is odd ({len(cleaned_hex)}). Appending '0' to align.")
        cleaned_hex += "0"
        
    try:
        binary_data = binascii.unhexlify(cleaned_hex)
        with open(bin_file, "wb") as f_out:
            f_out.write(binary_data)
        print(f"[SUCCESS] '{bin_file}' has been created successfully (size: {len(binary_data)} bytes)!")
    except Exception as e:
        print(f"[ERROR] Failed to convert: {e}")

if __name__ == "__main__":
    main()
