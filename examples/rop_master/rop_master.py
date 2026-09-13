import socket
import time

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", 50000))
    
    def send_cmd(cmd, delay=0.5):
        s.sendall(cmd.encode() + b'\n')
        time.sleep(delay)

    print("[*] Arming Architecturally Safe Pipeline...")
    send_cmd(".pagein ws2_32!recv", delay=2.0)
    send_cmd(".script examples/rop_master/rop_hybrid_extractor.ds")
    send_cmd("!pt enable pid 0")
    send_cmd("g")
    
    print("[*] Listening for ROP/JOP telemetry...")
    while True:
        data = s.recv(4096).decode(errors='ignore')
        if not data: break
        print(data, end='')
        
        if "INSTRUCTION-PERFECT STACK PIVOT DETECTED" in data:
            print("\n[*] Python Harness: Pivot caught! System is paused in VMX-Root.")
            
            # 1. Safely dump trace to disk using the Host KD engine
            print("[*] Automating Intel PT Extraction to Host File System...")
            send_cmd("!pt dump path C:\\traces\\master_rop_trace.pt type packet", delay=3.0)
            
            # 2. Safely rollback the system now that the trace is secure
            print("[*] Erasing exploit memory... Rolling back snapshot!")
            send_cmd("!snapshot restore")
            
            # 3. Resume the restored OS
            send_cmd("g")
            
            print("[+] Trace saved and target OS restored flawlessly. Extraction complete!")
            break

if __name__ == "__main__":
    main()
