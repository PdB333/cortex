// A tiny in-memory settings store (CommonJS).
const values = new Map([['theme', 'dark']]);

exports.get = (key) => values.get(key);
exports.set = (key, value) => {
  values.set(key, value);
  return true;
};
module.exports.size = () => values.size;
