import serial

ser = serial.Serial('/dev/serial0',9600,timeout=1)

print("Listening to serial port...\n")

while True:
    if ser.in_waiting:
        line = ser.readline().decode('utf-8').rstrip()
        if line:
            print(f"Received: {line}")
            # Here you can add code to process the received data
            # For example, you could send it to a web server or log it