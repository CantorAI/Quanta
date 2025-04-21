from Galaxy import galaxy
import embedding_api deferred

cantor = galaxy.cantor

class Quanta_Filter(galaxy.BaseFilter):
	inputPin:Pin = None
	outputPin:Pin = None
	pipelineId = ""
	description = ""
	model = ""
	embedder = None
	def Quanta_Filter():
		this.inputPin = this.NewInputPin()
		this.inputPin.setputcallback(this.OneFrame)
		this.outputPin = this.NewOutputPin()

	def OneFrame(frame):
		cantor = galaxy.cantor
		if this.embedder == None:
			embedding_api.load()
			embedder = embedding_api.EmbeddingAPI()
			embedder.set_active_model("mpnet")
		inputStr = frame.GetString()
		py_emb = this.embedder.embed_text(inputStr)
		emb = to_xlang(py_emb)
		rag_results = vdb.Lookup(emb, 2)
		results = str(rag_results,format = True)
		result_frame = galaxy.NewDataFrame()
		# keep same timestamp as input frame
		result_frame.NodeId_High = frame.NodeId_High
		result_frame.NodeId_Low = frame.NodeId_Low
		result_frame.startTime = frame.startTime
		result_frame.type = "quanta"
		# result_frame.format1 = frame.format1
		result_frame.data = results
		this.outputPin.put(result_frame)
		cantor.CantorStatHit("Quanta_Filter_FPS")
	
