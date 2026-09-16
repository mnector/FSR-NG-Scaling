import onnxruntime as ort
import numpy as np
import sys
import json
import traceback

# Pre-computed statistics for Depth-Anything V2
MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)

def load_model(model_path):
    """
    Load ONNX model with ROCm or DirectML provider.
    """
    try:
        # Try ROCm provider first (for AMD GPUs)
        providers = [
            ('ROCMExecutionProvider', {'device_id': 0}),
            ('CUDAExecutionProvider', {'device_id': 0}),
            ('DirectMLExecutionProvider', {'device_id': 0}),
            ('CPUExecutionProvider', {}),
        ]
        session = ort.InferenceSession(model_path, providers=providers)
        return session
    except Exception as e:
        print(f"Failed to load model: {e}", file=sys.stderr)
        return None

def infer_depth(session, rgb_b,):
    """
    Run depth inference.
    Args:
        session: ONNX InferenceSession
        rgb_np: float32 RGB tensor [3, 518, 518] normalized [0,1]
    Returns:
        depth: float32 depth map [518, 518] normalized [0,1]
    """
    try:
        input_name = session.get_inputs()[0].name
        output_name = session.get_outputs()[0].name

        # Input expects [1, 3, 518, 518]
        input_tensor = rgb_np[np.newaxis, ...]  # Add batch dimension

        # Run inference
        outputs = session.run([output_name], {input_name: input_tensor})
        depth = outputs[0][0]  # Remove batch dimension

        return depth.astype(np.float32)
    except Exception as e:
        print(f"Failed to run inference: {e}", file=sys.stderr)
        return None

def process_frame(frame_path, model_path):
    """
    Process a single frame for depth estimation.
    frame_path: path to binary frame data (RGB float32, interleaved, width*height)
    """
    import cv2
    
    # Load frame
    frame = cv2.imread(frame_path, cv2.IMREAD_COLOR)
    if frame is None:
        print("Failed to load frame", file=sys.stderr)
        return None

    # Convert BGR to RGB and normalize
    rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0

    # Resize to 518x518
    resized = cv2.resize(rgb, (518, 518), interpolation=cv2.INTER_LINEAR)

    # Normalize using ImageNet stats
    normalized = (resized - MEAN) / STD
    normalized = np.transpose(normalized, (2, 0, 1))  # CHW format

    # Load model
    session = load_model(model_path)
    if session is None:
        return None

    # Run inference
    depth = infer_depth(session, normalized)
    return depth

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python depth_infer.py <frame_path> <model_path>", file=sys.stderr)
        sys.exit(1)

    frame_path = sys.argv[1]
    model_path = sys.argv[2]
    output_path = sys.argv[3] if len(sys.argv) > 3 else None

    depth = process_frame(frame_path, model_path)
    if depth is not None:
        if output_path:
            # Save depth map (16-bit PNG)
            import cv2
            depth_normalized = (depth * 65535).astype(np.uint16)
            cv2.imwrite(output_path, depth_normalized)
            print(json.dumps({"status": "ok", "depth_shape": list(depth.shape)}))
        else:
            # Just return shape
            print(json.dumps({"status": "ok", "depth_shape": list(depth.shape)}))
    else:
        print(json.dumps({"status": "error"}), file=sys.stderr)
        sys.exit(1)
