"""Texture/image generation through Vertex AI (Gemini image models) using the gcloud ADC
credentials in %APPDATA%/gcloud/application_default_credentials.json (`gcloud auth application-default
login`; the quota project needs Vertex AI enabled).

  python tools/imagegen/gen.py models
  python tools/imagegen/gen.py gen "prompt" out.png [--model gemini-...-image] [--aspect 1:1]
"""
import argparse
import base64
import json
import os
import pathlib
import sys
import urllib.request

ADC = pathlib.Path(os.environ["APPDATA"]) / "gcloud" / "application_default_credentials.json"


def post(url, body, headers=None):
    data = json.dumps(body).encode() if not isinstance(body, bytes) else body
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code}: {e.read().decode(errors='replace')[:2000]}")


def get(url, headers):
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code}: {e.read().decode(errors='replace')[:2000]}")


def token():
    c = json.loads(ADC.read_text())
    body = urllib.parse.urlencode({"client_id": c["client_id"], "client_secret": c["client_secret"],
                                   "refresh_token": c["refresh_token"], "grant_type": "refresh_token"}).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=body)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["access_token"], c["quota_project_id"]


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("models")
    g = sub.add_parser("gen")
    g.add_argument("prompt"); g.add_argument("out")
    g.add_argument("--model", default="gemini-3-pro-image")
    g.add_argument("--aspect", default="1:1")
    g.add_argument("--size", default="1K", help="1K/2K/4K where supported")
    g.add_argument("--location", default="global")
    a = p.parse_args()
    tok, proj = token()
    hdr = {"Authorization": f"Bearer {tok}", "x-goog-user-project": proj}
    if a.cmd == "models":
        out = get("https://aiplatform.googleapis.com/v1beta1/publishers/google/models?pageSize=200&listAllVersions=true", hdr)
        for m in out.get("publisherModels", []):
            print(m.get("name"), m.get("versionId", ""), m.get("launchStage", ""))
        return
    host = "aiplatform.googleapis.com" if a.location == "global" else f"{a.location}-aiplatform.googleapis.com"
    url = f"https://{host}/v1/projects/{proj}/locations/{a.location}/publishers/google/models/{a.model}:generateContent"
    img_cfg = {"aspectRatio": a.aspect}
    if a.size:
        img_cfg["imageSize"] = a.size
    body = {"contents": [{"role": "user", "parts": [{"text": a.prompt}]}],
            "generationConfig": {"responseModalities": ["TEXT", "IMAGE"], "imageConfig": img_cfg}}
    out = post(url, body, hdr)
    n = 0
    for cand in out.get("candidates", []):
        for part in cand.get("content", {}).get("parts", []):
            if "inlineData" in part:
                path = a.out if n == 0 else a.out.replace(".png", f"_{n}.png")
                pathlib.Path(path).write_bytes(base64.b64decode(part["inlineData"]["data"]))
                print(f"wrote {path} ({part['inlineData'].get('mimeType')})")
                n += 1
            elif "text" in part:
                print("text:", part["text"][:500])
    if n == 0:
        print(json.dumps(out)[:2000])
        sys.exit(1)


if __name__ == "__main__":
    import urllib.parse
    main()
