// Module worker: wires PackHandler (handlers.js) to postMessage.
import { PackHandler } from './handlers.js';

const handler = new PackHandler();
let chain = Promise.resolve();
self.onmessage = (event) => {
  // one message at a time, in order (the handlers are async)
  chain = chain.then(() => handler.handle(event.data, (message, transfer) => self.postMessage(message, transfer ?? [])));
};
