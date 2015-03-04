import select
import socket
import sys
import threading
import urllib2
import copy
from time import time, sleep

UDP_SOCKET_TIMEOUT = 5
exitFlag = 0
exitValue = 0

def setExitFlag(n):
    global exitFlag
    exitFlag = n

class Data(object):
    """
    The data aggregated from the uwsgi app
    """
    def __init__(self):
        self.data = {}
        self.changed = {}
        self.isChanged = False
        self.dataLock = threading.Lock()

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
        self.dataLock.acquire()
        if name_value[0] in self.data and self.data[name_value[0]]['value'] != name_value[1]:
            self.changed[name_value[0]] = int(name_value[1])
            if name_value[0] == "myapp.worker.requests":
                self.isChanged = True
        self.data[name_value[0]] = metric
        self.dataLock.release()

    def new_packets(self, packets):
        packets = unicode(packets, 'utf-8', errors='replace')
        for packet in packets.splitlines():
            if not packet.strip():
                continue
            self.parse_packet(packet)

    def getChangedAttributes(self):
        self.dataLock.acquire()
        changedCopy = copy.deepcopy(self.changed)
        self.dataLock.release()
        return changedCopy

    def get_data(self):
        return self.data

    def ready(self):
        return self.isChanged

    def reset(self):
        self.isChanged = False

class Test(threading.Thread):
    """
    The class which trigger tests and check results
    """

    COUNT_METRICS = [
        'myapp.worker.requests',
        'myapp.worker.delta_requests',
        'myapp.worker.core.requests'
    ]

    INC_METRICS = [
        'myapp.worker.total_tx',
        'myapp.worker.respawns'
    ]

    VAL_METRICS = {
        'myapp.worker.avg_response_time': (50, 250)
    }

    def __init__(self, data):
        threading.Thread.__init__(self)
        self.data = data
        self.tests = 4
        self.success = 0
        self.failure = 0
        self.errors = []

    def setExitValue(self, val):
        global exitValue
        exitValue = val

    def check(self):
        attributes_changed = self.data.getChangedAttributes()

        if hasattr(self, 'oldData') and self.tests < 4:
            for k in self.COUNT_METRICS:
                if k in attributes_changed and k in self.oldData and attributes_changed[k] == self.oldData[k] + 1:
                    self.success += 1
                else:
                    self.setExitValue(1)
                    self.failure += 1
                    self.errors.append(k)

            for k in self.INC_METRICS:
                if k in attributes_changed and k in self.oldData and attributes_changed[k] >= self.oldData[k]:
                    self.success += 1
                else:
                    self.setExitValue(1)
                    self.failure += 1
                    self.errors.append(k)

            for k, v in self.VAL_METRICS.iteritems():
                if k in attributes_changed and k in self.oldData and v[0] <= attributes_changed[k] <= v[1]:
                    self.success += 1
                else:
                    self.setExitValue(1)
                    self.failure += 1
                    self.errors.append(k)

        self.oldData = attributes_changed

    def printResult(self):
        print "################################################################################"
        print "RESULTS"
        print "################################################################################"
        print ""
        print "SUCCESS: %d/%d" % (self.success, self.success + self.failure)
        print ""
        print "FAILURE: %d/%d" % (self.failure, self.success + self.failure)
        print ""
        print "################################################################################"
        print ""
        if self.failure > 0:
            print "Metrics failed:"
            for m in self.errors:
                print "* %s" % m
            print ""
            print "################################################################################"

    def run(self):
        print "TEST IN PROGRESS"
        sleep(10)
        self.check()
        while self.tests > 0:
            ready = 0
            timeout = 30
            test = urllib2.urlopen("http://localhost:9090").read()
            if test == "Hello World":
                while not ready and timeout > 0:
                    if self.data.ready():
                        ready = 1
                    timeout -= 1
                    sleep(1)

                if ready:
                    self.check()
                    self.data.reset()
                else:
                    print "Test failed: cannot aggregate metrics change"
            else:
                print "Error while testing, please check if the web application is running"
            self.tests -= 1
        self.printResult()
        setExitFlag(1)

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

def main():
    data = Data()
    server = Server(data)
    test = Test(data)
    server.start()
    test.start()
    while not exitFlag:
        pass
    server.join()
    test.join()
    print 'END TEST: Exiting'
    return exitValue

if __name__ == '__main__':
    sys.exit(main())
