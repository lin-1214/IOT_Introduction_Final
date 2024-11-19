import os
import sys
from dotenv import load_dotenv

load_dotenv(os.path.join(os.path.dirname(__file__), '..', '.env'))

if len(sys.argv) < 2:
    print("Usage: python3 NotifyAPI.py <message>")
    sys.exit(1)

zToken = os.getenv("ZTOKEN")
zMsg = sys.argv[1]
zCmd = f"curl -i -X POST -H 'Authorization: Bearer {zToken}' -d 'message={zMsg}' https://notify-api.line.me/api/notify"
os.system(zCmd)