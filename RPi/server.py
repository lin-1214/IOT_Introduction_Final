from flask import Flask, request, send_from_directory, jsonify
import os
import subprocess
from flask_cors import CORS
from dotenv import load_dotenv
import requests

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
        'config1': data.get('config1'),
        'config2': data.get('config2'),
        'config3': data.get('config3'),
        'config4': data.get('config4'),
        'config5': data.get('config5')
    }
    
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

        warning_test = "Warning: This is a test message"
        # run_subprocess(["python3", "notify.py", "-t", warning_test])
        
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
        
        x_position = data.get('xPosition')  # Relative position (0-1)
        stream_width = data.get('streamWidth')
        stream_id = data.get('streamId')
        
        # Validate input data
        if None in [x_position, stream_width, stream_id]:
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
        print(f"Sending position to ESP32 at {esp32_ip}")
        
        # Send position to ESP32
        try:
            esp32_url = f'http://{esp32_ip}/border'
            print(f"Making request to: {esp32_url}")
            
            # Updated payload to match ESP32 expectations
            payload = {
                'xPosition': x_position,
                'streamWidth': stream_width,
                'streamId': stream_id
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
        
        # Return success response with position data
        response_data = {
            "status": "success",
            "data": {
                "relativePosition": x_position,
                "absolutePosition": absolute_x,
                "streamWidth": stream_width,
                "esp32_ip": esp32_ip  # Include ESP32 IP for debugging
            }
        }
        print(f"Sending response: {response_data}")
        return jsonify(response_data), 200
        
    except Exception as e:
        print(f"Unexpected error: {str(e)}")
        return jsonify({
            "status": "error", 
            "message": f"Server error: {str(e)}"
        }), 500

if __name__ == '__main__':
    app.run(debug=True, port=8080, host='0.0.0.0')
    