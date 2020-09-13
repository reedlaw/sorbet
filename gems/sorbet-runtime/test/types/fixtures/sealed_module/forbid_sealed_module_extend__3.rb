# frozen_string_literal: true
module Opus::Types::Test::SealedModuleSandbox
  ChildBad4 = Class.new do
    extend Parent
  end
end
