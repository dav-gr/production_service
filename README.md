# PostProd Middleware Server

Simple TCP middleware for post-production barcode operations.

## Connection Model

**One-shot connections:** Connect → Send request → Receive response → Connection closed

No persistent connections. Each request is independent.

## Building

```bash
cmake --preset Qt-Debug
cmake --build out/build/debug
```

## Configuration

```ini
[Server]
Port=8080
ReadTimeoutMsec=5000

[Database]
Host=localhost
Port=5432
Database=prod_auto_dev
Username=prod_auto_dev
Password=prod_auto_dev

[Session]
ExpirationMinutes=480
```

## Running

```bash
PostProdMiddleware.exe
PostProdMiddleware.exe --config C:\path\to\config.ini
```

---

## Protocol

- **Transport:** TCP, port 8080
- **Format:** JSON, newline terminated (`\n`)
- **Encoding:** UTF-8

---

## API

### 1. Login

```json
{"action":"login","data":{"username":"admin","pin":"0000"}}
```

Response:
```json
{
  "status": "success",
  "message": "Login successful",
  "data": {
    "token": "abc123...",
    "user_id": 1,
    "username": "admin",
    "full_name": "Administrator",
    "permissions": ["postprod.remove_item", "postprod.unseal_box"]
  }
}
```

### 2. Logout

```json
{"action":"logout","data":{"token":"abc123..."}}
```

### 3. Validate Barcode

```json
{"action":"validate_barcode","data":{"token":"abc123...","barcode":"ITEM001"}}
```

Response (item):
```json
{
  "status": "success",
  "message": "Found",
  "data": {
    "barcode": "ITEM001",
    "entity_type": "item",
    "entity_id": 123,
    "status": 1,
    "in_box": true,
    "box_id": 10,
    "box_barcode": "BOX001"
  }
}
```

### 4. Process Barcode

Main operation. Behavior depends on scanned barcode:

| Scan | Action |
|------|--------|
| Item in box | Unseal box, remove ALL items, reset all to status=0 |
| Loose item (status=1, no box) | Reset item to status=0 |
| Box | Unseal box, remove ALL items, reset all to status=0 |

```json
{"action":"process_barcode","data":{"token":"abc123...","barcode":"ITEM001"}}
```

Response:
```json
{
  "status": "success",
  "message": "Box unsealed",
  "data": {
    "operation": "unseal_box",
    "barcode": "ITEM001",
    "entity_type": "item",
    "items_reset": 24,
    "box_barcode": "BOX001",
    "box_unsealed": true
  }
}
```

---

## Permissions Required

| Operation | Permission |
|-----------|------------|
| Reset item | `postprod.remove_item` |
| Unseal box | `postprod.unseal_box` |
| Item in box | Both required |

---

## Error Responses

```json
{"status":"error","message":"Invalid or expired session"}
{"status":"error","message":"Barcode not found","data":{"barcode":"XXX"}}
{"status":"error","message":"Permission denied: postprod.unseal_box"}
{"status":"error","message":"Item status must be 1","data":{"status":0}}
```

---

## Testing with netcat

```bash
# Connect and send login
echo '{"action":"login","data":{"username":"admin","pin":"0000"}}' | nc localhost 8080

# Process barcode (new connection)
echo '{"action":"process_barcode","data":{"token":"YOUR_TOKEN","barcode":"TEST"}}' | nc localhost 8080
```

---

## Project Structure

```
PostProdMiddleware/
├── CMakeLists.txt
├── config.ini.example
├── main.cpp
├── README.md
├── core/db/
│   ├── db_service.h
│   ├── db_service.cpp
│   └── types.h
└── server/
    ├── tcp_server.h
    ├── tcp_server.cpp
    ├── request_handler.h
    ├── request_handler.cpp
    └── protocol.h
```
