// Module worker: wires ScanHandler (handlers.js) to postMessage.
import { ScanHandler } from './handlers.js?v=0.3.4';

const handler = new ScanHandler();
let chain = Promise.resolve();
self.onmessage = (event) => {
  // one message at a time, in order (the handlers are async)
  chain = chain.then(() => handler.handle(event.data, (message, transfer) => self.postMessage(message, transfer ?? [])));
};
