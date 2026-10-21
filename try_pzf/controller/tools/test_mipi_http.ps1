param(
    [switch]$CompileOnly,
    [string]$Clang = 'C:/Espressif/tools/esp-clang/esp-19.1.2_20250312/esp-clang/bin/clang.exe',
    [string]$Linker = 'C:/Espressif/tools/esp-clang/esp-19.1.2_20250312/esp-clang/bin/ld.lld.exe',
    [string]$Python = 'C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
)
$ErrorActionPreference = 'Stop'
$controllerDir = Split-Path $PSScriptRoot -Parent
$source = Get-Content -Raw (Join-Path $controllerDir 'components/mipi_dsi_bridge/src/mipi_dsi_bridge.c')
$crcStart = $source.IndexOf('static uint16_t crc16_ccitt(')
$crcEnd = $source.IndexOf('/*---------------------------------------------------------------', $crcStart)
$helperStart = $source.IndexOf('typedef struct {')
$helperEnd = $source.IndexOf('/*---------------------------------------------------------------' + "`n" + ' * MCP tool callbacks - Camera control', $helperStart)
if ($helperEnd -lt 0) {
    $helperEnd = $source.IndexOf('/*---------------------------------------------------------------' + "`r`n" + ' * MCP tool callbacks - Camera control', $helperStart)
}
if ($crcStart -lt 0 -or $crcEnd -lt 0 -or $helperStart -lt 0 -or $helperEnd -lt 0) {
    throw 'Production helper boundaries not found; test must be updated.'
}
# Compile the production helpers, not a second implementation. No ESP board or
# host C runtime is needed: this fixture supplies deterministic HTTP mocks.
$fixture = @'
typedef unsigned long long size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef long long int64_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define UINT16_MAX 65535
#define PRId64 "lld"
typedef int esp_err_t;
typedef int esp_http_client_method_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_RESPONSE 5
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 2
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_EVENT_ON_DATA 2
#define FOCUSLAMP_ROLE_HEAD 1
#define MIPI_DSI_BRIDGE_HTTP_TIMEOUT_MS 10000
static int warnings;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) (++warnings)
static bool s_initialized = true;
typedef struct { const char *host; unsigned port; } focuslamp_endpoint_t;
typedef void *esp_http_client_handle_t;
typedef struct {
  int event_id; void *user_data; char *header_key; char *header_value;
  void *data; int data_len;
} esp_http_client_event_t;
typedef struct {
  const char *url; int method, timeout_ms;
  esp_err_t (*event_handler)(esp_http_client_event_t *);
  void *user_data; bool disable_auto_redirect;
} esp_http_client_config_t;
static esp_http_client_config_t active_config;
static int status = 200, transport_error;
static bool complete = true, chunked;
static const char *part1, *part2, *crc_value;
static bool peer_available = true, client_available = true;
static int64_t content_length;
size_t strlen(const char *s) { size_t n=0; while(s[n]) ++n; return n; }
void *memcpy(void *d, const void *s, size_t n) {
  char *out=d; const char *in=s; for(size_t i=0;i<n;++i) out[i]=in[i]; return d;
}
int strcasecmp(const char *a,const char *b) {
  for(;;++a,++b) { int x=*a,y=*b; if(x>='A'&&x<='Z') x+=32;
    if(y>='A'&&y<='Z') y+=32; if(x!=y||!x) return x-y; }
}
size_t strspn(const char *s,const char *set) {
  size_t n=0; for(;s[n];++n) { bool found=false;
    for(size_t j=0;set[j];++j) if(s[n]==set[j]) found=true;
    if(!found) break; } return n;
}
unsigned long strtoul(const char *s,char **end,int base) {
  unsigned long n=0; while(*s) { int d=*s-'0';
    if(*s>='a'&&*s<='f') d=*s-'a'+10;
    if(*s>='A'&&*s<='F') d=*s-'A'+10;
    if(d<0||d>=base) break; n=n*base+d; ++s; }
  *end=(char *)s; return n;
}
int snprintf(char *out,size_t cap,const char *fmt,...) {
  if(cap) out[0]=0; return 0;
}
static int focuslamp_discovery_get_peer(int role,focuslamp_endpoint_t *peer) {
  peer->host="head"; peer->port=80; return peer_available?ESP_OK:ESP_ERR_NOT_FOUND;
}
static void *esp_http_client_init(const esp_http_client_config_t *c) {
  active_config=*c; return client_available?(void *)1:NULL;
}
static int esp_http_client_set_header(void *c,const char *k,const char *v) { return ESP_OK; }
static int esp_http_client_set_post_field(void *c,const char *b,int n) { return ESP_OK; }
static int esp_http_client_perform(void *c) {
  esp_http_client_event_t event={0}; event.user_data=active_config.user_data;
  if(crc_value) { char temporary[32]; memcpy(temporary,crc_value,strlen(crc_value)+1);
    event.event_id=HTTP_EVENT_ON_HEADER; event.header_key="x-cRc16";
    event.header_value=temporary; active_config.event_handler(&event);
    temporary[0]='!'; /* response header lifetime ends after callback */
  }
  event.event_id=HTTP_EVENT_ON_DATA;
  if(part1) { event.data=(void *)part1; event.data_len=(int)strlen(part1);
    active_config.event_handler(&event); }
  if(part2) { event.data=(void *)part2; event.data_len=(int)strlen(part2);
    active_config.event_handler(&event); }
  return transport_error;
}
static int esp_http_client_get_status_code(void *c) { return status; }
static int64_t esp_http_client_get_content_length(void *c) { return content_length; }
static bool esp_http_client_is_complete_data_received(void *c) { return complete; }
static bool esp_http_client_is_chunked_response(void *c) { return chunked; }
static int esp_http_client_cleanup(void *c) { return ESP_OK; }
/* PRODUCTION_HELPERS */
#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
static bool same(const char *a,const char *b) {
  while(*a&&*a==*b) { ++a; ++b; } return *a==*b;
}
int run_tests(void) {
  char body[16]; int result;
  part1="{\"x\":"; part2="true}"; content_length=10;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK);
  CHECK(same(body,"{\"x\":true}")); CHECK(active_config.disable_auto_redirect);
  chunked=true; content_length=-1;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK);
  CHECK(same(body,"{\"x\":true}"));
  complete=false;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_INVALID_RESPONSE);
  CHECK(body[0]==0); complete=true;
  status=503; CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_INVALID_RESPONSE);
  status=200; transport_error=99;
  CHECK(http_get("/query",body,sizeof(body),true)==99); CHECK(body[0]==0);
  transport_error=0; part1="123456789012345"; part2=NULL;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(strlen(body)==15);
  part2="6";
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_INVALID_RESPONSE);
  CHECK(body[0]==0);
  CHECK(http_post("/command","{}",body,sizeof(body))==ESP_OK); CHECK(body[0]==0);
  CHECK(http_post("/command",NULL,NULL,0)==ESP_OK);
  CHECK(http_get("/query",NULL,0,true)==ESP_ERR_INVALID_ARG);
  CHECK(http_get("/query",body,0,true)==ESP_ERR_INVALID_ARG);
  part1="123456789"; part2=NULL; crc_value="29B1"; warnings=0;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(warnings==0);
  crc_value="0000"; warnings=0;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(warnings==1);
  crc_value="not-hex"; warnings=0;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(warnings==1);
  crc_value="123456789"; warnings=0;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(warnings==1);
  crc_value=NULL; part1=NULL; content_length=0; chunked=false;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_OK); CHECK(body[0]==0);
  peer_available=false;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_NOT_FOUND);
  peer_available=true; client_available=false;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_NO_MEM);
  client_available=true; s_initialized=false;
  CHECK(http_get("/query",body,sizeof(body),true)==ESP_ERR_INVALID_STATE);
  return 0;
}
'@
$production = $source.Substring($crcStart, $crcEnd - $crcStart) + $source.Substring($helperStart, $helperEnd - $helperStart)
$fixture = $fixture.Replace('/* PRODUCTION_HELPERS */', $production)
$artifactDir = Join-Path $controllerDir 'build/mipi_http_tests'
New-Item -ItemType Directory -Force $artifactDir | Out-Null
$objectFile = Join-Path $artifactDir 'http_fixture.obj'
$libraryFile = Join-Path $artifactDir 'http_fixture.dll'
if ($CompileOnly) {
    # The bundled Espressif clang only has RISC-V/Xtensa backends. Compile the
    # fixture for the board without claiming that its assertions were executed.
    $fixture | & $Clang --target=riscv32-unknown-elf -ffreestanding -fno-builtin -fno-stack-protector -x c -c - -o $objectFile
    if ($LASTEXITCODE -ne 0) { throw 'HTTP fixture compilation failed' }
    Write-Output 'HTTP fixture: compilation PASS; assertions NOT EXECUTED (no native compiler).'
    return
}
$targets = & $Clang --print-targets
if (($targets -join "`n") -notmatch 'x86') {
    throw 'This clang has no Windows native backend. Use -CompileOnly, or provide -Clang and -Linker paths for a native LLVM installation.'
}
$fixture | & $Clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector -x c -c - -o $objectFile
if ($LASTEXITCODE -ne 0) { throw 'HTTP fixture compilation failed' }
& $Linker -flavor link /dll /noentry /nodefaultlib /export:run_tests "/out:$libraryFile" $objectFile
if ($LASTEXITCODE -ne 0) { throw 'HTTP fixture linking failed' }
& $Python -c 'import ctypes, sys; lib = ctypes.CDLL(sys.argv[1]); result = lib.run_tests(); print("HTTP response tests:", "PASS" if result == 0 else "FAIL at fixture line " + str(result)); sys.exit(result != 0)' $libraryFile
if ($LASTEXITCODE -ne 0) { throw 'HTTP response tests failed' }
