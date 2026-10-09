// Module worker: wires ShaderHandler (handlers.js) to postMessage.
import { ShaderHandler } from './handlers.js?v=0.3.4';

const handler = new ShaderHandler();
let chain = Promise.resolve();
self.onmessage = (event) => {
  // one message at a time, in order (the handlers are async)
  chain = chain.then(() => handler.handle(event.data, (message, transfer) => self.postMessage(message, transfer ?? [])));
};
