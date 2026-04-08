import requests
from flask import Flask, render_template, request, Response

app = Flask(__name__)
CPP_BACKEND_URL = "http://localhost:8814"

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/<path:path>", methods=["GET", "POST", "PUT", "DELETE"])
def proxy(path):
    url = f"{CPP_BACKEND_URL}/api/{path}"
    
    # Forward the request to the C++ backend
    resp = requests.request(
        method=request.method,
        url=url,
        headers={key: value for (key, value) in request.headers if key != 'Host'},
        data=request.get_data(),
        cookies=request.cookies,
        allow_redirects=False)

    excluded_headers = ['content-encoding', 'content-length', 'transfer-encoding', 'connection']
    headers = [(name, value) for (name, value) in resp.raw.headers.items()
               if name.lower() not in excluded_headers]

    response = Response(resp.content, resp.status_code, headers)
    return response

if __name__ == "__main__":
    app.run(debug=True, port=5000)
