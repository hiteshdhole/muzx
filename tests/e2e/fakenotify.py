import asyncio
from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method
class N(ServiceInterface):
    def __init__(self): super().__init__('org.freedesktop.Notifications')
    @method()
    def Notify(self, app: 's', rid: 'u', icon: 's', summary: 's', body: 's', actions: 'as', hints: 'a{sv}', timeout: 'i') -> 'u':
        return 1
async def main():
    bus = await MessageBus().connect()
    bus.export('/org/freedesktop/Notifications', N())
    await bus.request_name('org.freedesktop.Notifications')
    await asyncio.get_event_loop().create_future()
asyncio.run(main())
