import os
import requests
import json
import sys

model_dir = os.path.join(
    os.path.dirname(os.path.dirname(__file__)),
    "models", "depth"
)
os.makedirs(model_dir, exist_ok=True)

# Depth-Anything V2 Small ONNX model
model_url = "https://huggingface.co/onnx-community/depth-anything-v2-small-ONNX/resolve/main/onnx/model.onnx"
model_file = os.path.join(model_dir, "depth_anything_v2_vits.onnx")

print(f"Downloading Depth-Anything-V2-Small ONNX model...")
print(f"URL: {model_url}")

try:
    response = requests.get(model_url, stream=True, timeout=120)
    response.raise_for_status()
    
    with open(model_file, "wb") as f:
        for chunk in response.iter_content(chunk_size=8192):
            if chunk:
                f.write(chunk)
    
    print(f"Model saved to: {model_file}")
    print(f"Model size: {os.path.getsize(model_file) / (1024*1024):.2f} MB")
    
except Exception as e:
    print(f"Error downloading model: {e}", file=sys.stderr)
    sys.exit(1)
