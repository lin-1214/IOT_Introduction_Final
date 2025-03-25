from flask import Flask, request, send_from_directory, jsonify
import os
import subprocess
from flask_cors import CORS
from dotenv import load_dotenv
import requests
import RPi.GPIO as GPIO
import time
import atexit

LED_PIN = 23
BUZZER_PIN = 24

# Set up GPIO
GPIO.setmode(GPIO.BCM)  # Use BCM pin numbering
GPIO.setup(LED_PIN, GPIO.OUT)  # Set up pin as output
GPIO.setup(BUZZER_PIN, GPIO.OUT)  # Set up pin as output

# Load environment variables
load_dotenv()

# Handle python script execution
def run_subprocess(command, working_dir='.', expected_returncode=0):
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=working_dir
    )
    if(result.stderr):
        print(result.stderr)
    assert result.returncode == expected_returncode
    return result.stdout.decode('utf-8')


app = Flask(__name__)
CORS(app)

# Get credentials from .env file
VALID_CREDENTIALS = {}
credentials = os.getenv('VALID_CREDENTIALS', 'admin:admin').split(':')
if len(credentials) == 2:
    VALID_CREDENTIALS[credentials[0]] = credentials[1]

# Get ESP32 IPs from .env file
ESP32_IPS = eval(os.getenv('ESP32_IP', '[]'))

# Simulated configuration storage
current_config = {}

# Serve static files
@app.route('/login.html')
def serve_login():
    return send_from_directory('static', 'login.html')

@app.route('/index.html')
def serve_index():
    return send_from_directory('static', 'index.html')

@app.route('/')
def serve_login_direct():
    return send_from_directory('static', 'login.html')

@app.route('/stream.html')
def serve_stream():
    return send_from_directory('static', 'stream.html')

# API endpoints
@app.route('/login', methods=['POST'])
def login():
    try:
        data = request.get_json()
        username = data.get('username')
        password = data.get('password')
        
        if not username or not password:
            return jsonify({"status": "error", "message": "Missing credentials"}), 400
        
        # Check if username exists and password matches
        if username == "admin" and password == "admin":
            return jsonify({"status": "success"}), 200
            
        return jsonify({"status": "error", "message": "Invalid username or password"}), 401
        
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route('/check-auth')
def check_auth():
    # In a real implementation, this would check session/token
    # For development, we'll just return success
    return jsonify({"status": "success"}), 200

@app.route('/save-config', methods=['POST'])
def save_config():
    data = request.get_json()
    global current_config
    
    current_config = {
        'config1': data.get('config1'),  # This is the confidence threshold
        'config2': data.get('config2'),
        'config3': data.get('config3'),
        'config4': data.get('config4'),
        'config5': data.get('config5')
    }
    
    # Send confidence threshold to all ESP32s
    confidence = data.get('config1')
    try:
        for esp32_ip in ESP32_IPS:
            esp32_url = f'http://{esp32_ip}/confidence'
            print(f"Sending confidence threshold {confidence} to ESP32 at {esp32_ip}")
            
            response = requests.post(
                esp32_url,
                json={'confidence': float(confidence)},
                timeout=5,
                headers={'Content-Type': 'application/json'}
            )
            
            if response.status_code != 200:
                print(f"Warning: ESP32 at {esp32_ip} returned status code {response.status_code}")
                
    except Exception as e:
        print(f"Error sending confidence to ESP32s: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500
    
    print("Saved configuration:", current_config)
    return jsonify({"status": "success"}), 200

# New route for ESP32 messages
@app.route('/esp32-message', methods=['POST', 'OPTIONS'])
def esp32_message():
    if request.method == 'OPTIONS':
        # Respond to preflight request
        response = jsonify({'status': 'ok'})
        response.headers.add('Access-Control-Allow-Origin', '*')
        response.headers.add('Access-Control-Allow-Headers', 'Content-Type')
        response.headers.add('Access-Control-Allow-Methods', 'POST')
        return response

    try:
        data = request.get_json()
        message = data.get('message')
        print(f"Received message from ESP32: {message}")

        # Trigger GPIO signal
        for i in range(3):
            GPIO.output(LED_PIN, GPIO.HIGH)  # Set pin high
            GPIO.output(BUZZER_PIN, GPIO.HIGH)  # Set pin high
            time.sleep(0.5)  # Keep signal high for 100ms
            GPIO.output(LED_PIN, GPIO.LOW)   # Set pin back to low
            GPIO.output(BUZZER_PIN, GPIO.LOW)  # Set pin back to low
            time.sleep(0.5)  # Keep signal low for 100ms
        
        # Send a proper response back to ESP32
        response = jsonify({"status": "success", "message": "Received your message"})
        response.headers.add('Access-Control-Allow-Origin', '*')
        return response, 200
    except Exception as e:
        print(f"Error: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route('/get-stream-urls')
def get_stream_urls():
    stream_urls = {
        f'stream{i+1}': f'http://{ip}:81/stream'
        for i, ip in enumerate(ESP32_IPS)
    }
    return jsonify(stream_urls)

@app.route('/save-line-position', methods=['POST'])
def save_line_position():
    try:
        # Log incoming request data
        data = request.get_json()
        print(f"Received line position request: {data}")
        
        stream_id = data.get('streamId')
        x_position = data.get('xPosition')  # Relative position (0-1)
        method = data.get('method')  # Get the method parameter
        
       
        
        # Validate input data
        if None in [stream_id, method]:
            return jsonify({
                "status": "error", 
                "message": "Missing required parameters"
            }), 400
            
        # Get the corresponding ESP32 IP based on stream ID
        esp32_index = int(stream_id.replace('stream', '')) - 1
        if esp32_index < 0 or esp32_index >= len(ESP32_IPS):
            return jsonify({
                "status": "error", 
                "message": f"Invalid stream ID: {stream_id}"
            }), 400
            
        esp32_ip = ESP32_IPS[esp32_index]
        print(f"Sending {method} command to ESP32 at {esp32_ip}")
        
        # Send position to ESP32
        try:
            esp32_url = f'http://{esp32_ip}/border'
            print(f"Making request to: {esp32_url}")
            
            # Updated payload to match ESP32 expectations
            payload = {
                'streamId': stream_id,
                'xPosition': x_position,
                'method': method
            }
            print(f"Sending payload: {payload}")
            
            response = requests.put(
                esp32_url,
                json=payload,
                timeout=5,
                headers={'Content-Type': 'application/json'}
            )
            
            print(f"ESP32 response status: {response.status_code}")
            print(f"ESP32 response body: {response.text}")
            
            if response.status_code != 200:
                raise Exception(f"ESP32 returned status code {response.status_code}: {response.text}")
                
        except requests.exceptions.ConnectionError as e:
            print(f"Connection error: {str(e)}")
            return jsonify({
                "status": "error", 
                "message": f"Could not connect to ESP32 at {esp32_ip}"
            }), 500
        except requests.exceptions.Timeout as e:
            print(f"Timeout error: {str(e)}")
            return jsonify({
                "status": "error", 
                "message": "ESP32 request timed out"
            }), 500
        except Exception as e:
            print(f"Error sending position to ESP32: {str(e)}")
            return jsonify({
                "status": "error", 
                "message": f"Failed to send position to ESP32: {str(e)}"
            }), 500
        
        
        return jsonify({"status": "success"}), 200
        
    except Exception as e:
        print(f"Unexpected error: {str(e)}")
        return jsonify({
            "status": "error", 
            "message": f"Server error: {str(e)}"
        }), 500

# Add cleanup handler at the end of the file
def cleanup():
    GPIO.cleanup()

atexit.register(cleanup)

if __name__ == '__main__':
    app.run(port=8080, host='0.0.0.0')
    
