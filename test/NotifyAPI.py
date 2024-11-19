import os
import sys
from dotenv import load_dotenv
from PIL import Image

def downscaleImage(zPic, zRatio, zQuality):
    zImg = Image.open(zPic)
    w, h = zImg.size
    zImg = zImg.resize((w // zRatio, h // zRatio), Image.Resampling.LANCZOS)
    zImg.save(zPic, quality=zQuality)
    return

# config
RATIO = 2
QUALITY = 75

load_dotenv(os.path.join(os.path.dirname(__file__), '..', '.env'))

if len(sys.argv) < 3:
    print("Usage: python3 NotifyAPI.py <-p/t> <picture file/text message>")
    sys.exit(1)

zToken = os.getenv("ZTOKEN")
zCmd = ""

if sys.argv[1] == "-p":
    zPic = sys.argv[2]
    downscaleImage(zPic, RATIO, QUALITY)
    zCmd = f"curl -i -X POST -H 'Authorization: Bearer {zToken}' -F 'message=Screenshot' -F 'imageFile=@{zPic}' https://notify-api.line.me/api/notify"
else:
    zMsg = sys.argv[2]
    zCmd = f"curl -i -X POST -H 'Authorization: Bearer {zToken}' -d 'message={zMsg}' https://notify-api.line.me/api/notify"


os.system(zCmd)