import http.client,json,datetime,urllib.request,sys
class PipeSocket:
 def __init__(self,p):self.p=p
 def makefile(self,*args):return self.p

def call(method,path):
 with open(r'\\.\pipe\verge-mihomo','r+b',buffering=0) as pipe:
  pipe.write((method+' '+path+' HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n').encode())
  response=http.client.HTTPResponse(PipeSocket(pipe));response.begin()
  return response.status,response.read()
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
if '--client-stopped' in sys.argv:
 frame={'success':False}  # Caller has explicitly stopped main-client first.
else:
 with opener.open('http://127.0.0.1:9000/frame',timeout=3) as response:frame=json.load(response)
if frame.get('success') and not frame.get('stale'):
 print('Video is already fresh; no connections touched.')
else:
 status,body=call('GET','/connections');assert status==200
 closed=0
 for c in json.loads(body).get('connections',[]):
  m=c.get('metadata',{})
  if m.get('destinationIP')!='10.23.38.143' or str(m.get('destinationPort'))!='80':continue
  # Camera upgrades have a tiny upload and a JPEG-sized download. Leave
  # ordinary REST/control connections and all other hosts alone.
  if not (c.get('download',0)>65536 and 0<c.get('upload',0)<1024):continue
  code,_=call('DELETE','/connections/'+c['id'])
  if code in (200,204):closed+=1
 print('Closed stale camera proxy connections:',closed)
