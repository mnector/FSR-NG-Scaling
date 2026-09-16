import sys
import os
import numpy as np
import onnxruntime as ort

# Pre-computed statistics for Depth-Anything V2
MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)

def resize_bilinear(src, src_h, src_w, dst_h, dst_w):
    """Resize RGB image (H*W*3) using bilinear interpolation."""
    dst = np.zeros((dst_h, dst_w, 3), dtype=np.float32)
    
    y_ratio = src_h / dst_h
    x_ratio = src_w / dst_w
    
    for y in range(dst_h):
        py = int(y * y_ratio)
        dy = (y * y_ratio) - py
        py1 = min(py + 1, src_h - 1)
        
        for x in range(dst_w):
            px = int(x * x_ratio)
            dx = (x * x_ratio) - px
            px1 = min(px + 1, src_w - 1)
            
            p00 = src[py, px]
            p01 = src[py, px1]
            p10 = src[py1, px]
            p11 = src[py1, px1]
            
            for c in range(3):
                dst[y, x, c] = (p00[c] * (1 - dx) * (1 - dy) +
                               p01[c] * dx * (1 - dy) +
                               p10[c] * (1 - dx) * dy +
                               p11[c] * dx * dy)
    return dst

def depth_inference(frame_path, output_path, model_path, provider="CPU"):
    """
    Run depth inference on a frame.
    Args:
        frame_path: Path to raw RGB float32 file (H*W*3 bytes)
        output_path: Path to save depth float32 file (H*W bytes)
        model_path: Path to ONNX model
        provider: "CPU" or "CUDA"
    """
    try:
        # Load frame (BGR float32 from DX12 -> RGB)
        frame = np.fromfile(frame_path, dtype=np.float32)
        h = int(np.sqrt(frame.size // 3))
        w = frame.size // 3 // h
        frame = frame.reshape(h, w, 3)
        
        # Convert BGR to RGB
        frame = frame[:, :, ::-1]
        
        # Resize to 518x518
        resized = resize_bilinear(frame, h, w, 518, 518)
        
        # Normalize
        normalized = (resized - MEAN) / STD
        normalized = np.transpose(normalized, (2, 0, 1))  # CHW
        
        # Add batch dimension
        input_tensor = normalized[np.newaxis, ...].astype(np.float32)
        
        # Load model
        providers = ['CUDAExecutionProvider', 'CPUExecutionProvider'] if provider == "CUDA" else ['CPUExecutionProvider']
        session = ort.InferenceSession(model_path, providers=providers)
        
        # Run inference
        output_name = session.get_outputs()[0].name
        outputs = session.run(None, {session.get_inputs()[0].name: input_tensor})
        depth = outputs[0][0]  # Remove batch dimension
        
        # Save depth (depth normalization: divide by max for [0,1] range)
        depth_normalized = depth / np.max(depth)
        depth_normalized.astype(np.float32).tofile(output_path)
        
        return {"status": "ok", "shape": list(depth.shape), "range": [float(depth.min()), float(depth.max())]}
        
    except Exception as e:
        return {"status": "error", "message": str(e)}

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python depth_infer.py <frame_path> <output_path> [model_path] [provider]")
        sys.exit(1)
    
    frame_path = sys.argv[1]
    output_path = sys.argv[2]
    model_path = sys.argv[3] if len(sys.argv) > 3 else r"C:\Users\mnect\.hermes\tools\FSR-NG-Scaling\models\depth\model.onnx"
    provider = sys.argv[4] if len(sys.argv) > 4 else "CPU"
    
    import json
    result = depth_inference(frame_path, output_path, model_path, provider)
    print(json.dumps(result))
