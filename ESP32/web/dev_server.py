from flask import Flask, request, send_from_directory, jsonify
import os

app = Flask(__name__)

# Simulated credentials - in real ESP32 these would be stored securely
VALID_CREDENTIALS = {
    "admin": "admin"
}

# Simulated configuration storage
current_config = {}

# Serve static files
@app.route('/')
@app.route('/index.html')
def serve_index():
    return send_from_directory('static', 'index.html')

@app.route('/login.html')
def serve_login():
    return send_from_directory('static', 'login.html')

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

if __name__ == '__main__':
    app.run(debug=True, port=8080)