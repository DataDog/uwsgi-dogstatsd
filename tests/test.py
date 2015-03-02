import select
import socket
import sys
import threading
import urllib2
from time import time, sleep

UDP_SOCKET_TIMEOUT = 5
exitFlag = 0
dataLock = threading.Lock()

class Data(object):
    """
    The data aggregated from the uwsgi app
    """

    def __init__(self):
        self.data = {}

    def parse_packet(self, packet):
        tags = None
        metadata = packet.split('|')
        if (len(metadata) < 2):
            raise Exception('Unparseable metric packet: %s' % packet)
    
        name_value = metadata[0].split(':')
        metric_type = metadata[1]
        if (len(metadata) == 3):
            tags = metadata[2].split(',')
            if (len(tags) < 1 or not tags[0].startswith('#')):
                raise Exception('Unparseable metric packet: %s' % packet)
            tags[0] = tags[0][1:]

        if (len(name_value) != 2): 
            raise Exception('Unparseable metric packet: %s' % packet)

        
        metric = {
            'name': name_value[0],
            'value': name_value[1],
            'metric_type': metric_type,
            'tags': tags
        }
        self.data[name_value[0]] = metric

    def new_packets(self, packets):
        packets = unicode(packets, 'utf-8', errors='replace')
        for packet in packets.splitlines():
            if not packet.strip():
                continue

            self.parse_packet(packet)
        
    def get_data(self):
        return self.data

class Test(threading.Thread):
    """
    The class which trigger tests and check results
    """
    def __init__(self, data):
        threading.Thread.__init__(self)
        self.data = data

    def run(self):
        while not exitFlag:
            print "test"
            sleep(3)

class Server(threading.Thread):
    """
    The process which will listen on the statd port
    """
    config = {
        'host': 'localhost',
        'port': 8125
    }

    def __init__(self, data):
        threading.Thread.__init__(self)
        self.data = data
        self.buffer_size = 1024 * 8
        self.address = (self.config['host'], self.config['port'])

    def run(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setblocking(0)
        try:
            self.socket.bind(self.address)
        except socket.gaierror:
            if self.address[0] == 'localhost':
                log.warning("Warning localhost seems undefined in your host file, using 127.0.0.1 instead")
                self.address = ('127.0.0.1', self.address[1])
                self.socket.bind(self.address)

        print "Listening on host & port: %s" % str(self.address)

        sock = [self.socket]
        select_select = select.select
        timeout = UDP_SOCKET_TIMEOUT
  
        while not exitFlag:
            try:
                ready = select_select(sock, [], [], timeout)
                if ready[0]:
                    message = self.socket.recv(self.buffer_size)
                    self.data.new_packets(message)
            except Exception:
                print 'Error receiving datagram'

def main(server):
    server.start()
    i = 5

data = Data()
server = Server(data)
test = Test(data)
#main(server)
server.start()
test.start()
i = 5
while i > 0:
    i -= 1
    print i
    sleep(1)
exitFlag = 1
server.join()
test.join()
print 'Exiting'


#if __name__ == '__main__':
#    sys.exit(main())
