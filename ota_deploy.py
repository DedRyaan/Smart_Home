import os
import sys
import socket
import subprocess
from concurrent.futures import ThreadPoolExecutor

# Configurations
TARGET_MAC = "30-76-f5-f4-21-9c" # Your ESP32's unique MAC address
MAIN_CPP_PATH = os.path.join("src", "main.cpp")

REQUIRED_PATTERNS = {
    "#include <ArduinoOTA.h>": "OTA header inclusion",
    "ArduinoOTA.begin()": "OTA initialization in setup()",
    "ArduinoOTA.handle()": "OTA handler execution in loop()"
}

# Known Espressif MAC prefixes (OUI)
ESPRESSIF_PREFIXES = ["30-76-f5", "24-d7-eb", "3c-61-05", "30-ae-a4", "84-0d-8e", "bc-dd-c2"]

def get_local_subnets():
    """Detects all IPv4 addresses on local interfaces and returns their subnet prefixes."""
    subnets = set()
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None):
            if info[0] == socket.AF_INET:
                ip = info[4][0]
                if not ip.startswith("127."):
                    parts = ip.split(".")
                    if len(parts) == 4:
                        subnets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.")
    except Exception:
        pass
    
    if not subnets:
        subnets.add("192.168.1.")
        
    return list(subnets)

def ping_ip(ip):
    """Pings an IP address with a short timeout to populate the ARP cache."""
    try:
        subprocess.run(
            ["ping", "-n", "1", "-w", "200", ip], 
            stdout=subprocess.DEVNULL, 
            stderr=subprocess.DEVNULL
        )
    except Exception:
        pass

def populate_arp_table(subnets):
    """Sweeps all detected subnets to populate the system ARP cache."""
    print("[*] Performing quick network ping sweep to refresh ARP cache...")
    ips = []
    for subnet in subnets:
        ips.extend([f"{subnet}{i}" for i in range(1, 255)])
        
    with ThreadPoolExecutor(max_workers=100) as executor:
        executor.map(ping_ip, ips)

def find_esp32_ip_by_mac():
    """Parses the ARP table to locate the ESP32's IP address based on its MAC."""
    res = subprocess.run(["arp", "-a"], capture_output=True, text=True)
    
    # Check for our exact target MAC address first
    for line in res.stdout.splitlines():
        if TARGET_MAC in line.lower():
            parts = line.split()
            if parts:
                return parts[0], f"specific MAC match ({TARGET_MAC})"
                
    # Fallback: search for any Espressif MAC address prefix
    espressif_devices = []
    for line in res.stdout.splitlines():
        for prefix in ESPRESSIF_PREFIXES:
            if prefix in line.lower():
                parts = line.split()
                if parts and parts[0] not in [d[0] for d in espressif_devices]:
                    espressif_devices.append((parts[0], parts[1]))
                    
    if espressif_devices:
        if len(espressif_devices) == 1:
            return espressif_devices[0][0], f"generic Espressif OUI match ({espressif_devices[0][1]})"
        else:
            print(f"[!] Multiple Espressif devices found: {espressif_devices}")
            print(f"[*] Defaulting to the first device: {espressif_devices[0][0]}")
            return espressif_devices[0][0], f"first Espressif OUI match ({espressif_devices[0][1]})"
            
    return None, None

def validate_code_standards():
    """Validates that the required OTA code blocks are present in main.cpp."""
    print("[*] Validating code standards in src/main.cpp...")
    if not os.path.exists(MAIN_CPP_PATH):
        print(f"[ERROR] Could not find {MAIN_CPP_PATH}!")
        return False
        
    with open(MAIN_CPP_PATH, "r", encoding="utf-8") as f:
        content = f.read()
        
    errors = []
    for pattern, description in REQUIRED_PATTERNS.items():
        if pattern not in content:
            errors.append(f"Missing {description} ('{pattern}')")
            
    if errors:
        print("\n[!] CODE VALIDATION FAILED! The following OTA code blocks are missing:")
        for err in errors:
            print(f"    - {err}")
        print("\n[!] Upload aborted to prevent disabling wireless updates.")
        return False
        
    print("[+] Code validation passed! OTA requirements are present.")
    return True

def run_platformio_upload(ip):
    """Executes the PlatformIO OTA upload command targeting the discovered IP."""
    print(f"[*] Starting wireless upload to ESP32 at {ip}...")
    
    # Locate platformio executable
    pio_path = os.path.join(".venv", "Scripts", "pio.exe")
    if not os.path.exists(pio_path):
        pio_path = "pio" # Fallback to global PATH
        
    command = [
        pio_path, "run", 
        "-e", "esp32dev_ota", 
        "--target", "upload", 
        "--upload-port", ip
    ]
    
    try:
        process = subprocess.Popen(
            command, 
            stdout=subprocess.PIPE, 
            stderr=subprocess.STDOUT, 
            text=True,
            bufsize=1
        )
        
        for line in process.stdout:
            print(line, end="")
            
        process.wait()
        return process.returncode == 0
    except Exception as e:
        print(f"[ERROR] Failed to execute upload command: {e}")
        return False

def main():
    # 1. Validate source code standards
    if not validate_code_standards():
        sys.exit(1)
        
    # 2. Discover ESP32 IP
    subnets = get_local_subnets()
    populate_arp_table(subnets)
    
    target_ip, match_reason = find_esp32_ip_by_mac()
    
    if not target_ip:
        print("[ERROR] No ESP32 running OTA was found on your local network.")
        print("Ensure the ESP32 is powered on and connected to the same Wi-Fi network.")
        sys.exit(1)
        
    print(f"[+] Found ESP32 running OTA at IP: {target_ip} ({match_reason})")
    
    # 3. Perform wireless upload
    success = run_platformio_upload(target_ip)
    if success:
        print("\n[+] Wireless upload completed successfully!")
    else:
        print("\n[ERROR] Wireless upload failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
