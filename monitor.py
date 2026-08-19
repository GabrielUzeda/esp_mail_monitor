import time
import argparse
import re
import logging
from logging.handlers import RotatingFileHandler
import io
import sys

# Mocking serial for environments without it
class NativeSerial:
    def __init__(self, port, baud, timeout=1):
        import subprocess
        self.port = port
        subprocess.run(["stty", "-F", port, str(baud), "cs8", "-cstopb", "-parenb", "raw", "-echo"], check=False)
        self.file = open(port, "rb", buffering=0)
    def readline(self):
        line = bytearray()
        while True:
            char = self.file.read(1)
            if not char:
                break
            line.extend(char)
            if char == b'\n':
                break
        return bytes(line)
    def setDTR(self, val): pass
    def setRTS(self, val): pass
    def close(self):
        if hasattr(self, 'file'):
            self.file.close()

try:
    import serial
    from serial.serialutil import SerialException
except ImportError:
    serial = type("serial", (object,), {"Serial": NativeSerial})

# Regex pattern for error detection (incluindo crashes ESP32 e falhas do resumo Gemini)
ERROR_PATTERN = re.compile(r"(Guru Meditation Error|StoreProhibited|InstrFetchProhibited|panic|Core \d panic|ERROR:|Exception \(\d+\)|_Nenhum conteúdo baixado_|_Erro ao enviar|Erro Analise Gemini|Erro na requisição)")

def setup_logging():
    log_file = "/tmp/esp32_crash.log"
    logger = logging.getLogger("ESP32Monitor")
    logger.setLevel(logging.INFO)
    
    # 1MB per file, keep 3 backup files
    handler = RotatingFileHandler(log_file, maxBytes=1024*1024, backupCount=3)
    formatter = logging.Formatter('%(asctime)s - %(levelname)s - %(message)s')
    handler.setFormatter(formatter)
    logger.addHandler(handler)
    
    # Also log to console
    console = logging.StreamHandler()
    console.setFormatter(formatter)
    logger.addHandler(console)
    
    return logger

def monitor(ser, logger):
    print(f"--- Monitor serial iniciado ---", flush=True)
    while True:
        try:
            line = ser.readline()
            if not line:
                continue
            
            decoded_line = line.decode('utf-8', errors='replace').rstrip()
            print(decoded_line, flush=True)
            
            if ERROR_PATTERN.search(decoded_line):
                logger.error(f"Detected error: {decoded_line}")
                
        except Exception as e:
            print(f"Serial error: {e}. Retrying in 5 seconds...", flush=True)
            time.sleep(5)
            continue
        except KeyboardInterrupt:
            print("Monitoramento encerrado.", flush=True)
            break

def run_test(logger):
    print("--- Executando Modo de Teste ---", flush=True)
    test_data = [
        "Normal operation trace",
        "Guru Meditation Error: Some crash info",
        "Just a normal log",
        "ERROR: something went wrong",
        "StoreProhibited at 0x40001234"
    ]
    
    # Simulate serial with StringIO
    mock_input = io.StringIO("\n".join(test_data))
    
    for line in mock_input:
        line = line.rstrip()
        print(f"Simulating: {line}", flush=True)
        if ERROR_PATTERN.search(line):
            logger.error(f"Detected error: {line}")
        else:
            logger.info(f"Normal: {line}")
    print("--- Teste concluído ---", flush=True)

def main():
    parser = argparse.ArgumentParser(description="ESP32 Serial Monitor")
    parser.add_argument('--port', default='/dev/ttyUSB0', help='Serial port')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--test', action='store_true', help='Run in test mode')
    args = parser.parse_args()
    
    logger = setup_logging()
    
    if args.test:
        run_test(logger)
        return

    while True:
        try:
            ser = serial.Serial(args.port, args.baud, timeout=1)
            print(f"--- Resetando ESP32 na {args.port} ---", flush=True)
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.2)
            ser.setRTS(False)
            time.sleep(0.2)
            
            monitor(ser, logger)
            break
            
        except Exception as e:
            print(f"Could not open {args.port}: {e}. Retrying in 5 seconds...", flush=True)
            time.sleep(5)

if __name__ == "__main__":
    main()
