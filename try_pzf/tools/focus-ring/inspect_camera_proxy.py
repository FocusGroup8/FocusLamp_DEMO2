import http.client,json
class PipeSocket:
    def __init__(self,p):self.p=p
    def makefile(self,*args):return self.p
with open(r'\\.\pipe\verge-mihomo','r+b',buffering=0) as pipe:
    pipe.write(b'GET /connections HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
    response=http.client.HTTPResponse(PipeSocket(pipe));response.begin()
    print('Clash API HTTP',response.status)
    body=json.loads(response.read())
    for c in body.get('connections',[]):
        m=c.get('metadata',{})
        if m.get('destinationIP')=='10.23.38.143' or m.get('host')=='10.23.38.143':
            print(json.dumps({'id':c.get('id'),'start':c.get('start'),'chains':c.get('chains'),'download':c.get('download'),'upload':c.get('upload'),'metadata':{k:m.get(k) for k in ['sourceIP','sourcePort','destinationIP','destinationPort','host','process','processPath','type']}},ensure_ascii=False))
