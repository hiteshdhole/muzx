import asyncio, sys, time
from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method, dbus_property, PropertyAccess
from dbus_next import Variant
name, title, artist, url, status, start = sys.argv[1:7]
t0 = time.time(); base = float(start)
class Root(ServiceInterface):
    def __init__(self): super().__init__('org.mpris.MediaPlayer2')
    @dbus_property(access=PropertyAccess.READ)
    def Identity(self) -> 's': return name
class Player(ServiceInterface):
    def __init__(self): super().__init__('org.mpris.MediaPlayer2.Player')
    @dbus_property(access=PropertyAccess.READ)
    def PlaybackStatus(self) -> 's': return status
    @dbus_property(access=PropertyAccess.READ)
    def Metadata(self) -> 'a{sv}':
        m = {'mpris:trackid': Variant('o', '/t/1'), 'xesam:title': Variant('s', title),
             'mpris:length': Variant('x', 200000000)}
        if artist: m['xesam:artist'] = Variant('as', [artist])
        if url: m['xesam:url'] = Variant('s', url)
        return m
    @dbus_property(access=PropertyAccess.READ)
    def Volume(self) -> 'd': return 0.5
    @dbus_property(access=PropertyAccess.READ)
    def Position(self) -> 'x':
        el = (time.time() - t0) if status == 'Playing' else 0
        return int((base + el) * 1e6)
async def main():
    bus = await MessageBus().connect()
    bus.export('/org/mpris/MediaPlayer2', Root()); bus.export('/org/mpris/MediaPlayer2', Player())
    await bus.request_name('org.mpris.MediaPlayer2.' + name)
    await asyncio.get_event_loop().create_future()
asyncio.run(main())
