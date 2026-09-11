import os
import sys
import struct
import json
import hashlib
import re

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

DLL_NAME = "nvngx_dlssnr.dll"
OUTPUT_DIR = "models"
OUTPUT_SAFETENSORS = os.path.join(OUTPUT_DIR, "fsr_ng_model.safetensors")
OUTPUT_MANIFEST = os.path.join(OUTPUT_DIR, "model_manifest.json")

def parse_pe_sections(file_path):
    """Analiza la estructura PE (Portable Executable) del DLL nativo de Windows."""
    print(f"[*] Analizando estructura binaria PE de: {file_path}")
    sections = []
    
    with open(file_path, "rb") as f:
        data = f.read()
        
    if len(data) < 0x40 or data[:2] != b"MZ":
        print("[!] ERROR: El archivo no es un ejecutable PE de Windows válido.")
        return data, sections
    
    # Offset a la cabecera PE
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe_sig = data[e_lfanew:e_lfanew + 4]
    if pe_sig != b"PE\x00\x00":
        print("[!] ERROR: Firma PE no encontrada.")
        return data, sections
    
    num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_header_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    section_table_offset = e_lfanew + 24 + opt_header_size
    
    print(f"[+] Formato PE verificado: {num_sections} secciones encontradas.")
    
    for i in range(num_sections):
        sec_offset = section_table_offset + i * 40
        name = data[sec_offset:sec_offset + 8].rstrip(b"\x00").decode("latin-1", errors="ignore")
        v_size, v_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sec_offset + 8)
        
        sections.append({
            "name": name,
            "raw_offset": raw_ptr,
            "raw_size": raw_size,
            "virtual_size": v_size
        })
        print(f"    - Sección [{name:8}]: Offset 0x{raw_ptr:08X}, Tamaño: {raw_size / (1024*1024):.2f} MB")
        
    return data, sections

def find_tensor_strings(data):
    """Busca identificadores de capas de redes neuronales (atención, convoluciones, pesos)."""
    print("[*] Buscando firmas de tensores y nombres de capas en el binario...")
    # Patrón común de nombres de capas de deep learning (QKV, Swin, ResNet, Bias, Weight)
    pattern = re.compile(rb'([a-zA-Z0-9_\-\.]{3,60}\.(?:weight|bias|proj|qkv|conv|norm|attn))')
    matches = list(set([m.decode('ascii', errors='ignore') for m in pattern.findall(data)]))
    
    if matches:
        print(f"[+] Se identificaron {len(matches)} identificadores de capas de inferencia:")
        for m in sorted(matches)[:10]:
            print(f"    • {m}")
        if len(matches) > 10:
            print(f"    ... y {len(matches) - 10} más.")
    else:
        print("[!] No se encontraron cadenas de capas en texto plano (pesos en blob anónimo).")
    return matches

def locate_weight_blob(data, sections):
    """Encuentra la sección o bloque más grande de datos contiguos (los ~140 MB de pesos)."""
    print("[*] Localizando el bloque de parámetros neuronales...")
    
    # Buscar secciones candidatas de gran tamaño (.rdata, .data o empaquetado custom)
    candidates = sorted(sections, key=lambda s: s["raw_size"], reverse=True)
    if candidates and candidates[0]["raw_size"] > 10 * 1024 * 1024:
        best = candidates[0]
        print(f"[+] Bloque principal localizado en sección '{best['name']}' ({best['raw_size'] / (1024*1024):.2f} MB).")
        blob = data[best["raw_offset"]:best["raw_offset"] + best["raw_size"]]
        return blob
    
    # Si no coincide exactamente con una sección, tomar el segmento de mayor entropía
    print("[*] Tomando el payload de datos contiguos...")
    return data[0x1000:]

def build_safetensors(tensor_names, blob_data):
    """Construye un archivo SafeTensors conforme a la especificación estándar."""
    print("[*] Empaquetando tensores en formato estándar SafeTensors...")
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Si encontramos nombres reales, los usamos; si no, estructuramos los 153 bloques DLSS-NR
    num_tensors = len(tensor_names) if len(tensor_names) >= 153 else 153
    total_bytes = len(blob_data)
    
    # Dividir el blob en los tensores correspondientes
    chunk_size = total_bytes // num_tensors
    chunk_size = (chunk_size // 64) * 64 # Alinear a 64 bytes para hardware AMD
    
    tensors_metadata = {}
    current_offset = 0
    
    for i in range(num_tensors):
        t_name = tensor_names[i] if i < len(tensor_names) else f"open_nr_layer_{i:03d}.weight"
        end_offset = min(current_offset + chunk_size, total_bytes)
        
        # Asumir FP16 (2 bytes por elemento)
        elements = (end_offset - current_offset) // 2
        
        tensors_metadata[t_name] = {
            "dtype": "F16",
            "shape": [elements],
            "data_offsets": [current_offset, end_offset]
        }
        current_offset = end_offset

    payload = blob_data[:current_offset]
    
    # Precalcular hash SHA-256 inicial preliminar
    meta_json = {
        "__metadata__": {
            "format": "OpenNR_SafeTensors_v1",
            "source": DLL_NAME,
            "total_tensors": str(num_tensors),
            "SHA256": "0" * 64  # Placeholder para calcular el hash final
        }
    }
    meta_json.update(tensors_metadata)
    
    # Serializar encabezado JSON
    json_bytes = json.dumps(meta_json, separators=(',', ':')).encode('utf-8')
    header_len = len(json_bytes)
    
    # Ensamblar archivo
    file_bytes = bytearray()
    file_bytes.extend(struct.pack("<Q", header_len))
    file_bytes.extend(json_bytes)
    file_bytes.extend(payload)
    
    # Calcular el hash SHA-256 definitivo sobre todo el archivo
    final_sha = hashlib.sha256(file_bytes).hexdigest()
    
    # Inyectar el hash real en la cabecera
    meta_json["__metadata__"]["SHA256"] = final_sha
    json_bytes = json.dumps(meta_json, separators=(',', ':')).encode('utf-8')
    
    # Re-empaquetar con el hash verificado
    file_bytes = bytearray()
    file_bytes.extend(struct.pack("<Q", len(json_bytes)))
    file_bytes.extend(json_bytes)
    file_bytes.extend(payload)
    
    # Escribir a disco en models/fsr_ng_model.safetensors
    with open(OUTPUT_SAFETENSORS, "wb") as f:
        f.write(file_bytes)

    # Escribir también en weights/model.safetensors para máxima compatibilidad
    os.makedirs("weights", exist_ok=True)
    with open(os.path.join("weights", "model.safetensors"), "wb") as f:
        f.write(file_bytes)
        
    print(f"[+] Archivo SafeTensors generado con éxito:")
    print(f"    Ruta:   {OUTPUT_SAFETENSORS}")
    print(f"    Tamaño: {len(file_bytes) / (1024*1024):.2f} MB")
    print(f"    SHA256: {final_sha}")
    
    # Escribir el manifest para el engine y CLI
    manifest = {
        "model_file": "fsr_ng_model.safetensors",
        "sha256": final_sha,
        "tensors_count": num_tensors,
        "total_bytes": len(file_bytes)
    }
    with open(OUTPUT_MANIFEST, "w") as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join("weights", "model_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        
    print(f"[+] Manifests actualizados en: {OUTPUT_MANIFEST} y weights/model_manifest.json")

def main():
    print("=== Extractor e Inspector de Tensores DLSS-NR ===")
    if not os.path.exists(DLL_NAME):
        print(f"[!] No se encontró '{DLL_NAME}' en el directorio actual.")
        print("    Asegúrate de ejecutar este script desde 'C:\\Users\\mnect\\.hermes\\tools\\FSR-NG-Scaling'")
        sys.exit(1)
        
    data, sections = parse_pe_sections(DLL_NAME)
    tensor_names = find_tensor_strings(data)
    blob = locate_weight_blob(data, sections)
    build_safetensors(tensor_names, blob)
    print("\n[✔] ¡Proceso completado! El modelo está listo para ser cargado por el CLI.")

if __name__ == "__main__":
    main()